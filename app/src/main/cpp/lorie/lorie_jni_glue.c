/**
 * @file lorie_jni_glue.c
   * @brief Android JNI 统一胶水与事件路由层 (实现第 1、2 步)
 *
 * 职责：
 * 1. 挂载所有来自 Android Java ）。 JNI 桥接并触 Surface 发中性平入台层注入）。
 * 2. 桥接并触发中性平台层 (termux_hybrid_platform.cpp) 的全局初始化。
   */
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <stdint.h>
#include <stdlib.h>

#define LOG_TAG "Lorie_JNIGlue"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ----------------------------------------------------------------------------
// 声明在 termux_hybrid_platform.cpp 中实现的 C++ 原生方法
// ----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
    void* init_android_platform(void* native_window);
    void global_platform_inject_key(int32_t keycode, int32_t action);
    void global_platform_inject_pointer(int32_t x, int32_t y, int32_t action, int32_t button_mask);
#ifdef __cplusplus
}
#endif

// ----------------------------------------------------------------------------
// JNI 始口实现：处理 Android Activity / View 的 Surface 绑定
// ----------------------------------------------------------------------------

/**
 * @brief 当 Android 端的 SurfaceView/TextureView 准备就绪时触发
   * Java 对应类可能为 com.termux.x11.LorieView 或 LorieActivity
 */
JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_setSurface(JNIEnv* env, jobject thiz, jobject surface) {
      if (surface == NULL) {
        LOGI("Surface is NULL, releasing native platform reference");
        // 可以在此处添加释放平台窗口的逻辑
        return;
      }

    // 1. 在取 Android 原始 Surface 对应的 ANativeWindow 句柄
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == NULL) {
        LOGE("Failed to get ANativeWindow from Java Surface!");
        return;
    }

    LOGI("Successfully acquired ANativeWindow. Initializing hybrid platform...");

    // 2. 调用 C++ 层的中性接口初始化，托管底层渲染和调度管道
    void* platform_ptr = init_android_platform((void*)window);
    if (platform_ptr == NULL) {
        LOGE("Failed to initialize android_platform_t!");
    } else {
        LOGI("android_platform_t initialization completed successfully.");
    }
}

// ----------------------------------------------------------------------------
// JNI 接口实现：接管并分发 Android 端的原始硬件输入事件
// ----------------------------------------------------------------------------

/**
 * @brief 接收并路由键盘事件
 */
JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendKeyEvent(JNIEnv* env, jobject thiz, jint keycode, jint action) {
      // 将输入流统一导向 global_platform，由其根据件前协议焦点的状态，分发给 X11 或 Wayland
    global_platform_inject_key((int32_t)keycode, (int32_t)action);
}

/**
 * @brief 当收并路由触控和鼠标指针事件
 */
JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendMouseEvent(JNIEnv* env, jobject thiz, jint x, jint y, jint action, jint button_mask) {
      // 统一分发至抽象层
    global_platform_inject_pointer((int32_t)x, (int32_t)y, (int32_t)action, (int32_t)button_mask);
}
