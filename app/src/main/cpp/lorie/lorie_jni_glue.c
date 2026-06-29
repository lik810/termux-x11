/**
 * @file lorie_jni_glue.c
 * @brief Android JNI 双协议统一剪贴板、输入法与 DPI 同步胶合层 (对齐 scale 动态接口)
 */

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#define LOG_TAG "Lorie_JNIGlue"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif
    void* init_android_platform(void* native_window);
    bool start_wayland_compositor(const char* socket_dir);
    void register_jni_clipboard_callback(JNIEnv* env, jobject lorie_view_obj);
    void update_native_clipboard(const char* text);
    void global_platform_inject_text_input(const char* utf8_text);
    void global_platform_inject_key(int32_t keycode, int32_t action);
    void global_platform_inject_pointer(int32_t x, int32_t y, int32_t action, int32_t button_mask);

    void platform_set_output_scale(void* self, float scale_x, float scale_y);

    void* g_platform_instance_ptr = NULL;
#ifdef __cplusplus
}
#endif

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_setSurface(JNIEnv* env, jobject thiz, jobject surface) {
    if (surface == NULL) {
        LOGI("Surface is NULL, releasing resources.");
        return;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == NULL) {
        LOGE("Failed to acquire ANativeWindow!");
        return;
    }

    LOGI("Acquired ANativeWindow. Launching unified backend platforms...");

    g_platform_instance_ptr = init_android_platform((void*)window);
    if (g_platform_instance_ptr == NULL) {
        LOGE("Platform initialization failed.");
        return;
    }

    register_jni_clipboard_callback(env, thiz);

    const char* socket_path = "/data/data/com.termux/files";
    if (start_wayland_compositor(socket_path)) {
        LOGI("Embedded Wayland Engine successfully started!");
    } else {
        LOGE("Failed to start Embedded Wayland Engine.");
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_onClipboardChanged(JNIEnv* env, jobject thiz, jstring text) {
    if (text == NULL) return;
    const char* utf_text = (*env)->GetStringUTFChars(env, text, NULL);
    if (utf_text) {
        update_native_clipboard(utf_text);
        (*env)->ReleaseStringUTFChars(env, text, utf_text);
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendTextInput(JNIEnv* env, jobject thiz, jstring text) {
    if (text == NULL) return;
    const char* utf_text = (*env)->GetStringUTFChars(env, text, NULL);
    if (utf_text) {
        global_platform_inject_text_input(utf_text);
        (*env)->ReleaseStringUTFChars(env, text, utf_text);
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_onDpiScaleChanged(JNIEnv* env, jobject thiz, jfloat scale_x, jfloat scale_y) {
    if (g_platform_instance_ptr) {
        platform_set_output_scale(g_platform_instance_ptr, (float)scale_x, (float)scale_y);
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendKeyEvent(JNIEnv* env, jobject thiz, jint keycode, jint action) {
    global_platform_inject_key((int32_t)keycode, (int32_t)action);
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendMouseEvent(JNIEnv* env, jobject thiz, jint x, jint y, jint action, jint button_mask) {
    global_platform_inject_pointer((int32_t)x, (int32_t)y, (int32_t)action, (int32_t)button_mask);
}
