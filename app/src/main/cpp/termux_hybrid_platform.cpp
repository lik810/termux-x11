/**
 * @file termux_hybrid_platform.cpp
 * @brief 双协议混合显示服务平台抽象层（添加标准输出调试与多路 bind 尝试）
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <jni.h>
#include <stdio.h>
#include <android/log.h>
#include <android/native_window.h>

#ifndef TAG
#define TAG "HybridPlatform"
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define DBG_PRINT(fmt, ...) { \
    fprintf(stdout, "[Platform-DEBUG] " fmt "\n", ##__VA_ARGS__); \
    fflush(stdout); \
    LOGI(fmt, ##__VA_ARGS__); \
}

#define DBG_ERR(fmt, ...) { \
    fprintf(stderr, "[Platform-ERROR] " fmt " (errno: %d, msg: %s)\n", ##__VA_ARGS__, errno, strerror(errno)); \
    fflush(stderr); \
    LOGE(fmt, ##__VA_ARGS__); \
}

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} platform_rect_t;

typedef enum {
    PLATFORM_KEY_DOWN,
    PLATFORM_KEY_UP
} platform_key_action_t;

typedef enum {
    PLATFORM_POINTER_DOWN,
    PLATFORM_POINTER_UP,
    PLATFORM_POINTER_MOVE,
    PLATFORM_POINTER_SCROLL
} platform_pointer_action_t;

typedef enum {
    PLATFORM_CURSOR_ARROW,
    PLATFORM_CURSOR_IBEAM,
    PLATFORM_CURSOR_HAND,
    PLATFORM_CURSOR_NONE
} platform_cursor_type_t;

typedef enum {
    FOCUS_X11,
    FOCUS_WAYLAND
} protocol_focus_t;

typedef struct android_platform {
    void (*present_frame)(struct android_platform* self, void* buffer, int32_t stride, 
                          const platform_rect_t* damages, int32_t damage_count);
    void (*set_frame_limit)(struct android_platform* self, int32_t target_fps);
    void (*wait_vblank)(struct android_platform* self);
    void (*set_clipboard_text)(struct android_platform* self, const char* text);
    char* (*get_clipboard_text)(struct android_platform* self);
    void (*set_cursor_style)(struct android_platform* self, platform_cursor_type_t type);
    void (*set_output_scale)(struct android_platform* self, float scale_x, float scale_y);

    ANativeWindow* native_window;
    protocol_focus_t current_focus;
    pthread_mutex_t render_mutex;
    pthread_mutex_t clip_mutex;
    char* clipboard_cache;
    float scale_x;
    float scale_y;
} android_platform_t;

static android_platform_t* g_platform = NULL;
static JavaVM* g_jvm = NULL;
static jobject g_lorie_view_global_ref = NULL;
static jmethodID g_set_clipboard_method = NULL;

static void android_impl_present_frame(android_platform_t* self, void* buffer, int32_t stride, 
                                       const platform_rect_t* damages, int32_t damage_count) {
    if (!self || !self->native_window || !buffer) return;
    pthread_mutex_lock(&self->render_mutex);
    ANativeWindow_Buffer window_buffer;
    if (ANativeWindow_lock(self->native_window, &window_buffer, NULL) < 0) {
        pthread_mutex_unlock(&self->render_mutex);
        return;
    }
    uint32_t* dst = (uint32_t*)window_buffer.bits;
    uint32_t* src = (uint32_t*)buffer;
    int h = window_buffer.height < 1080 ? window_buffer.height : 1080;
    int w = window_buffer.width < 1920 ? window_buffer.width : 1920;
    for (int y = 0; y < h; y++) {
        memcpy(dst + y * window_buffer.stride, src + y * (stride / 4), w * 4);
    }
    ANativeWindow_unlockAndPost(self->native_window);
    pthread_mutex_unlock(&self->render_mutex);
}

static void android_impl_set_frame_limit(android_platform_t* self, int32_t target_fps) {}
static void android_impl_wait_vblank(android_platform_t* self) {
    static struct timespec last_time = {0, 0};
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    long long elapsed_ns = (current_time.tv_sec - last_time.tv_sec) * 1000000000LL + 
                           (current_time.tv_nsec - last_time.tv_nsec);
    long long target_ns = 1000000000LL / 60;
    if (elapsed_ns < target_ns) {
        struct timespec delay;
        delay.tv_sec = 0;
        delay.tv_nsec = target_ns - elapsed_ns;
        nanosleep(&delay, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &last_time);
}

static void android_impl_set_clipboard_text(android_platform_t* self, const char* text) {
    if (!self || !text) return;
    pthread_mutex_lock(&self->clip_mutex);
    if (self->clipboard_cache) free(self->clipboard_cache);
    self->clipboard_cache = strdup(text);
    pthread_mutex_unlock(&self->clip_mutex);

    if (g_jvm && g_lorie_view_global_ref && g_set_clipboard_method) {
        JNIEnv* env = NULL;
        jint res = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
        bool is_attached = false;
        if (res == JNI_EDETACHED) {
            if (g_jvm->AttachCurrentThread(&env, NULL) == 0) is_attached = true;
        }
        if (env) {
            jstring jstr = env->NewStringUTF(text);
            env->CallVoidMethod(g_lorie_view_global_ref, g_set_clipboard_method, jstr);
            env->DeleteLocalRef(jstr);
        }
        if (is_attached) g_jvm->DetachCurrentThread();
    }
}

static char* android_impl_get_clipboard_text(android_platform_t* self) {
    if (!self) return strdup("");
    pthread_mutex_lock(&self->clip_mutex);
    char* ret = self->clipboard_cache ? strdup(self->clipboard_cache) : strdup("");
    pthread_mutex_unlock(&self->clip_mutex);
    return ret;
}

static void android_impl_set_cursor_style(android_platform_t* self, platform_cursor_type_t type) {}
static void android_impl_set_output_scale(android_platform_t* self, float scale_x, float scale_y) {
    if (!self) return;
    self->scale_x = scale_x;
    self->scale_y = scale_y;
}

// ============================================================================
// Wayland UNIX Socket 管理核心
// ============================================================================

typedef struct {
    pthread_t thread;
    bool is_running;
    int server_fd;
    int client_fd;
    void* shm_data;
    size_t shm_size;
} inline_wayland_server_t;

static inline_wayland_server_t g_wl_server = {0, false, -1, -1, NULL, 0};

static int recv_fd_from_client(int socket) {
    struct msghdr msg = {0};
    char buf[1];
    struct iovec io = { .iov_base = buf, .iov_len = sizeof(buf) };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);
    ssize_t received = recvmsg(socket, &msg, 0);
    if (received <= 0) return -1;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int fd;
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        return fd;
    }
    return -1;
}

static void send_wayland_event(int client_fd, uint32_t object_id, uint16_t opcode, const void* data, uint16_t data_size) {
    uint32_t header[2];
    header[0] = object_id;
    uint32_t total_size = 8 + data_size;
    total_size = (total_size + 3) & ~3;
    header[1] = (total_size << 16) | opcode;
    send(client_fd, header, 8, 0);
    if (data_size > 0 && data != NULL) {
        send(client_fd, data, data_size, 0);
        uint32_t pad = 0;
        int padding_bytes = total_size - (8 + data_size);
        if (padding_bytes > 0) send(client_fd, &pad, padding_bytes, 0);
    }
}

static void* wayland_server_thread_func(void* arg);

static void* wayland_server_thread_func(void* arg) {
    DBG_PRINT("Wayland thread active and ready to accept connections.");
    while (g_wl_server.is_running) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_wl_server.server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (g_wl_server.is_running) usleep(100000);
            continue;
        }
        DBG_PRINT("Wayland socket successfully accepted a new client!");
        g_wl_server.client_fd = client_fd;
        struct {
            uint32_t id;
            uint32_t str_len;
            char name[16]; 
            uint32_t version;
        } __attribute__((packed)) global_compositor_event = { 1, 13, "wl_compositor", 4 };
        send_wayland_event(client_fd, 2, 0, &global_compositor_event, sizeof(global_compositor_event));

        uint32_t buffer[1024];
        while (g_wl_server.is_running && g_wl_server.client_fd != -1) {
            int shm_fd = recv_fd_from_client(client_fd);
            if (shm_fd != -1) {
                g_wl_server.shm_size = 1920 * 1080 * 4;
                if (g_wl_server.shm_data != NULL) munmap(g_wl_server.shm_data, g_wl_server.shm_size);
                g_wl_server.shm_data = mmap(NULL, g_wl_server.shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
                close(shm_fd);
                if (g_wl_server.shm_data == MAP_FAILED) {
                    g_wl_server.shm_data = NULL;
                } else {
                    if (g_platform) g_platform->current_focus = FOCUS_WAYLAND;
                }
            }
            if (g_wl_server.shm_data != NULL && g_platform != NULL && g_platform->current_focus == FOCUS_WAYLAND) {
                platform_rect_t damages = {0, 0, 1920, 1080};
                g_platform->present_frame(g_platform, g_wl_server.shm_data, 1920 * 4, &damages, 1);
            }
            usleep(16000);
        }
    }
    return NULL;
}

// ============================================================================
// 5. C++ -> C 符号导出
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

extern void x11_on_key_event(int32_t keycode, platform_key_action_t action);
extern void x11_on_pointer_event(int32_t x, int32_t y, platform_pointer_action_t action, int32_t button_mask);
extern void x11_on_text_input(const char* utf8_text);

void* init_android_platform(void* native_window) {
    if (g_platform != NULL) {
        g_platform->native_window = (ANativeWindow*)native_window;
        return (void*)g_platform;
    }
    android_platform_t* platform = (android_platform_t*)malloc(sizeof(android_platform_t));
    if (platform == NULL) return NULL;
    memset(platform, 0, sizeof(android_platform_t));
    platform->native_window = (ANativeWindow*)native_window;
    platform->current_focus = FOCUS_X11;
    platform->scale_x = 1.0f;
    platform->scale_y = 1.0f;
    pthread_mutex_init(&platform->render_mutex, NULL);
    pthread_mutex_init(&platform->clip_mutex, NULL);
    platform->present_frame = android_impl_present_frame;
    platform->set_frame_limit = android_impl_set_frame_limit;
    platform->wait_vblank = android_impl_wait_vblank;
    platform->set_clipboard_text = android_impl_set_clipboard_text;
    platform->get_clipboard_text = android_impl_get_clipboard_text;
    platform->set_cursor_style = android_impl_set_cursor_style;
    platform->set_output_scale = android_impl_set_output_scale;
    g_platform = platform;
    return (void*)platform;
}

void register_jni_clipboard_callback(JNIEnv* env, jobject lorie_view_obj) {
    env->GetJavaVM(&g_jvm);
    g_lorie_view_global_ref = env->NewGlobalRef(lorie_view_obj);
    jclass lorie_class = env->GetObjectClass(lorie_view_obj);
    g_set_clipboard_method = env->GetMethodID(lorie_class, "setAndroidClipboard", "(Ljava/lang/String;)V");
}

void update_native_clipboard(const char* text) {
    if (!g_platform || !text) return;
    pthread_mutex_lock(&g_platform->clip_mutex);
    if (g_platform->clipboard_cache) free(g_platform->clipboard_cache);
    g_platform->clipboard_cache = strdup(text);
    pthread_mutex_unlock(&g_platform->clip_mutex);
}

void global_platform_inject_text_input(const char* utf8_text) {
    if (!g_platform || !utf8_text) return;
    if (g_platform->current_focus == FOCUS_X11) {
        x11_on_text_input(utf8_text);
    } else if (g_platform->current_focus == FOCUS_WAYLAND) {
        if (g_wl_server.client_fd != -1) {
            uint32_t len = strlen(utf8_text);
            struct { uint32_t serial; uint32_t str_len; } __attribute__((packed)) text_header = { 0, len };
            send_wayland_event(g_wl_server.client_fd, 5, 0, &text_header, sizeof(text_header));
            send(g_wl_server.client_fd, utf8_text, len, 0);
            uint32_t pad = 0;
            int padding = ((len + 3) & ~3) - len;
            if (padding > 0) send(g_wl_server.client_fd, &pad, padding, 0);
        }
    }
}

bool start_wayland_compositor(const char* socket_dir) {
    if (g_wl_server.is_running) {
        DBG_PRINT("Wayland compositor server is already running.");
        return true;
    }

    DBG_PRINT("Initializing Wayland UNIX socket inside path: %s", socket_dir);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        DBG_ERR("Failed to create UNIX socket descriptor.");
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/wayland-0", socket_dir);
    unlink(addr.sun_path); 

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        DBG_ERR("Failed to bind UNIX socket to path: %s", addr.sun_path);
        close(fd);
        return false;
    }

    if (listen(fd, 5) < 0) {
        DBG_ERR("Failed to put socket in listening state.");
        close(fd);
        return false;
    }

    g_wl_server.server_fd = fd;
    g_wl_server.is_running = true;

    setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    DBG_PRINT("Successfully listening on Wayland socket: %s", addr.sun_path);

    if (pthread_create(&g_wl_server.thread, NULL, wayland_server_thread_func, NULL) != 0) {
        DBG_ERR("Failed to create worker thread for socket handling.");
        g_wl_server.is_running = false;
        close(fd);
        return false;
    }

    return true;
}

void global_platform_inject_key(int32_t keycode, int32_t action) {
    if (!g_platform) return;
    platform_key_action_t plat_action = (action == 0) ? PLATFORM_KEY_DOWN : PLATFORM_KEY_UP;
    if (g_platform->current_focus == FOCUS_X11) {
        x11_on_key_event(keycode, plat_action);
    } else if (g_platform->current_focus == FOCUS_WAYLAND) {
        if (g_wl_server.client_fd != -1) {
            uint32_t key_data[4] = { 0, (uint32_t)time(NULL), (uint32_t)keycode, (uint32_t)action };
            send_wayland_event(g_wl_server.client_fd, 3, 2, key_data, sizeof(key_data));
        }
    }
}

void global_platform_inject_pointer(int32_t x, int32_t y, int32_t action, int32_t button_mask) {
    if (!g_platform) return;
    platform_pointer_action_t plat_action;
    switch(action) {
        case 0: plat_action = PLATFORM_POINTER_DOWN; break;
        case 1: plat_action = PLATFORM_POINTER_UP; break;
        default: plat_action = PLATFORM_POINTER_MOVE; break;
    }
    if (g_platform->current_focus == FOCUS_X11) {
        x11_on_pointer_event(x, y, plat_action, button_mask);
    } else if (g_platform->current_focus == FOCUS_WAYLAND) {
        if (g_wl_server.client_fd != -1) {
            struct { uint32_t serial; uint32_t time_ms; uint32_t fixed_x; uint32_t fixed_y; } __attribute__((packed)) motion_event = {
                0, (uint32_t)time(NULL), (uint32_t)(x << 8), (uint32_t)(y << 8)
            };
            send_wayland_event(g_wl_server.client_fd, 4, 1, &motion_event, sizeof(motion_event));
        }
    }
}

#ifdef __cplusplus
}
#endif
