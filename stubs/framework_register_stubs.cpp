// PF-arch-005 (2026-05-11): no-op stub definitions for the
// register_android_* functions that PF-arch-004 declared as weak externs.
//
// Architecture rule (CLAUDE.md): no per-app branches. These are GENERIC
// stubs that satisfy the linker so dalvikvm's PF-arch-004 registration
// loop completes. Each function returns 0 (success). They DO NOT register
// any native methods — that's done by OHBridge / framework_native_stubs
// for the classes we actually support. The point of these stubs is to
// stop the boot from SIGBUSing on weak-undefined references during
// framework.jar class resolution.
//
// As specific classes become important, replace the corresponding stub
// here with a real implementation that does FindClass + RegisterNatives.

#include <jni.h>

extern "C" {
// True C symbols (the platform expects unmangled names for these).
int register_android_graphics_classes(JNIEnv*) { return 0; }
int register_android_functions(JNIEnv*) { return 0; }
}  // extern "C"

// Binder + Process live at global scope but with C++ mangling (linker symbol
// `_Z26register_android_os_BinderP7_JNIEnv` etc.). They are NOT inside
// `namespace android`. The asm-directive call sites in runtime.cc PF-arch-004
// match this exact mangling.
int register_android_os_Binder(JNIEnv*) { return 0; }
int register_android_os_Process(JNIEnv*) { return 0; }

// Namespaced symbols. The platform's libandroid_runtime puts most of these
// in namespace android — we replicate that here so the mangling matches.
namespace android {

int register_android_os_Parcel(JNIEnv*) { return 0; }
int register_android_os_SystemProperties(JNIEnv*) { return 0; }
int register_android_os_SystemClock(JNIEnv*) { return 0; }
int register_android_os_Trace(JNIEnv*) { return 0; }
int register_android_os_Debug(JNIEnv*) { return 0; }
int register_android_os_MessageQueue(JNIEnv*) { return 0; }
int register_android_os_ServiceManager(JNIEnv*) { return 0; }
int register_android_os_ServiceManagerNative(JNIEnv*) { return 0; }
int register_android_content_AssetManager(JNIEnv*) { return 0; }
int register_android_content_res_ApkAssets(JNIEnv*) { return 0; }
int register_android_content_StringBlock(JNIEnv*) { return 0; }
int register_android_content_XmlBlock(JNIEnv*) { return 0; }
int register_android_content_res_Configuration(JNIEnv*) { return 0; }
int register_android_view_Surface(JNIEnv*) { return 0; }
int register_android_view_SurfaceControl(JNIEnv*) { return 0; }
int register_android_view_SurfaceSession(JNIEnv*) { return 0; }
int register_android_view_InputDevice(JNIEnv*) { return 0; }
int register_android_view_InputChannel(JNIEnv*) { return 0; }
int register_android_view_KeyEvent(JNIEnv*) { return 0; }
int register_android_view_MotionEvent(JNIEnv*) { return 0; }
int register_android_view_WindowManagerGlobal(JNIEnv*) { return 0; }
int register_android_app_Activity(JNIEnv*) { return 0; }
int register_android_app_ActivityThread(JNIEnv*) { return 0; }
int register_android_animation_PropertyValuesHolder(JNIEnv*) { return 0; }
int register_android_database_SQLiteConnection(JNIEnv*) { return 0; }
int register_android_database_CursorWindow(JNIEnv*) { return 0; }
int register_android_util_Log(JNIEnv*) { return 0; }

}  // namespace android
