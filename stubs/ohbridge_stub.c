/*
 * OHBridge JNI stub — minimal no-op for standalone builds.
 * The real OHBridge is in the OHOS shim layer (arkui_bridge.cpp).
 * This stub just returns JNI_VERSION_1_6 from JNI_OnLoad.
 *
 * Entry point: JNI_OnLoad (renamed to JNI_OnLoad_ohbridge via -D)
 */
#include <jni.h>
#include <stdio.h>

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    fprintf(stderr, "[STUB] OHBridge JNI_OnLoad called (no-op stub)\n");
    fflush(stderr);
    return JNI_VERSION_1_6;
}
