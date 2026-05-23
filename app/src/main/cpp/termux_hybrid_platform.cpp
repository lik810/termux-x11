/**
 * @file termux_hybrid_platform.cpp
 * @brief Termux-X11 & Wayland 双协议统一平台后端重构核心 (修复 C/C++ 符号链接问题)
 *
 * 架构目标：
 * 1. 抽离原 X11 DDX 中直接耦合 Android 层的逻辑，建立中性平台接口 (android_platform_t)。
 * 2. 保持原 X11 功能不受影响，但其输入、渲染、剪贴板、帧调度完全重构为走此中性接口。
 * 3. 预留共享的 帧提交/Damage 合并、Frame Pacing、输入分发、剪贴板通道，供后续 libweston (Wayland) 无缝接入。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

// ============================================================================
// 1. 统一底层数据结构定义
// ============================================================================

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

// ============================================================================
// 2. 核心中性平台接口：android_platform_t
// ============================================================================

typedef struct android_platform {
    // ---- 帧提交与调度后端 (Frame Pacing & Damage) ----
    /**
     * @brief 提交一帧画面，支持局部脏区域更新 (Damage 合并)
     * @param buffer 渲染后的像素数据缓冲区
     * @param stride 每一行的字节数
     * @param damages 局部更新的矩形区域数组
     * @param damage_count 矩形计数
     */
    void (*present_frame)(struct android_platform* self, void* buffer, int32_t stride, 
                          const platform_rect_t* damages, int32_t damage_count);

    /**
     * @brief 设置帧率限制。0 表示不限制，>0 为目标帧率限制值 (如 60, 120)
     */
    void (*set_frame_limit)(struct android_platform* self, int32_t target_fps);

    /**
     * @brief 等待 VBlank 同步 (用于 Frame Pacing 帧同步)
     */
    void (*wait_vblank)(struct android_platform* self);

    // ---- 系统剪贴板交互 ----
    void (*set_clipboard_text)(struct android_platform* self, const char* text);
    char* (*get_clipboard_text)(struct android_platform* self);

    // ---- 光标与 UI 缩放 ----
    void (*set_cursor_style)(struct android_platform* self, platform_cursor_type_t type);
    void (*set_output_scale)(struct android_platform* self, float scale_x, float scale_y);

    // ---- 输入事件分发上层回调 ----
    void (*on_key_event)(int32_t keycode, platform_key_action_t action);
    void (*on_pointer_event)(int32_t x, int32_t y, platform_pointer_action_t action, int32_t button_mask);
    void (*on_text_input)(const char* utf8_text);

    // 内部私有 Android 上下文
    void* android_context;
} android_platform_t;

// 全局唯一的 Android 平台抽象实例
static android_platform_t* g_platform = NULL;

// ============================================================================
// 3. Android 平台具体实现适配器 (JNI/Native 粘合层)
// ============================================================================

static void android_impl_present_frame(android_platform_t* self, void* buffer, int32_t stride, 
                                       const platform_rect_t* damages, int32_t damage_count) {
    if (damage_count > 0 && damages != NULL) {
        for (int32_t i = 0; i < damage_count; ++i) {
            // 局部脏矩形更新逻辑留空，待后续与 Android SurfaceView 双缓冲队列对接
        }
    }
}

static void android_impl_set_frame_limit(android_platform_t* self, int32_t target_fps) {
    // 动态帧同步周期控制
}

static void android_impl_wait_vblank(android_platform_t* self) {
    static struct timespec last_time = {0, 0};
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    long long elapsed_ns = (current_time.tv_sec - last_time.tv_sec) * 1000000000LL + 
                           (current_time.tv_nsec - last_time.tv_nsec);
    long long target_ns = 1000000000LL / 60; // 默认 60 FPS 同步阈值

    if (elapsed_ns < target_ns) {
        struct timespec delay;
        delay.tv_sec = 0;
        delay.tv_nsec = target_ns - elapsed_ns;
        nanosleep(&delay, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &last_time);
}

static void android_impl_set_clipboard_text(android_platform_t* self, const char* text) {
    // 剪贴板输入
}

static char* android_impl_get_clipboard_text(android_platform_t* self) {
    return strdup("");
}

static void android_impl_set_cursor_style(android_platform_t* self, platform_cursor_type_t type) {
    // 鼠标样式
}

static void android_impl_set_output_scale(android_platform_t* self, float scale_x, float scale_y) {
    // DPI 缩放
}

// ============================================================================
// 4. C++ -> C 符号导出（解决 Undefined Symbol 链接报错的核心部分）
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化全局中性平台抽象。
 * 通过 extern "C" 包裹，强迫 C++ 编译器不进行名字修饰，从而生成能被 C 语言 (lorie_jni_glue.c) 链接的符号。
 */
void* init_android_platform(void* native_window) {
    if (g_platform != NULL) {
        return (void*)g_platform;
    }

    android_platform_t* platform = (android_platform_t*)malloc(sizeof(android_platform_t));
    if (platform == NULL) {
        return NULL;
    }
    memset(platform, 0, sizeof(android_platform_t));

    platform->android_context = native_window;
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

// ============================================================================
// 5. 统一的输入与平台管理中心 (双协议事件路由转发)
// ============================================================================

// X11 事件分发适配器声明 (对应于原 Xorg 核心事件)
void x11_on_key_event(int32_t keycode, platform_key_action_t action) {
    // 转发给 X11 驱动
}

void x11_on_pointer_event(int32_t x, int32_t y, platform_pointer_action_t action, int32_t button_mask) {
    // 转发给 X11 驱动
}

/**
 * @brief 当 Android 键盘事件传入时触发，将其路由至正确的上层协议栈
 */
void global_platform_inject_key(int32_t keycode, int32_t action) {
    if (!g_platform) return;

    platform_key_action_t plat_action = (action == 0) ? PLATFORM_KEY_DOWN : PLATFORM_KEY_UP;

    // 默认分发给 X11，后续接通 Wayland 时，根据前台焦点状态切换
    x11_on_key_event(keycode, plat_action);
}

/**
 * @brief 当 Android 指针事件传入时触发，路由至对应的协议栈
 */
void global_platform_inject_pointer(int32_t x, int32_t y, int32_t action, int32_t button_mask) {
    if (!g_platform) return;

    platform_pointer_action_t plat_action;
    switch(action) {
        case 0: plat_action = PLATFORM_POINTER_DOWN; break;
        case 1: plat_action = PLATFORM_POINTER_UP; break;
        default: plat_action = PLATFORM_POINTER_MOVE; break;
    }

    x11_on_pointer_event(x, y, plat_action, button_mask);
}

#ifdef __cplusplus
}
#endif

