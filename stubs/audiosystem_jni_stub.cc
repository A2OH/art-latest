// SPDX-License-Identifier: Apache-2.0
//
// art-latest/stubs/audiosystem_jni_stub.cc — Westlake M5-PRE
//
// Statically linked JNI stubs for android.media.AudioSystem's
// native methods. Purpose:
//   Unblock AudioSystem.<clinit>'s `native_getMaxChannelCount` lookup so
//   that any noice code touching AudioSystem (notification sound,
//   MediaSession init, AudioFocus, etc.) does not NPE.
//
// In W2-discover and M4-PRE7/M4-PRE8 runs, `AudioSystem.<clinit>` hit
// `UnsatisfiedLinkError` on `native_getMaxChannelCount`; ART logs
// "Tolerating clinit failure for Landroid/media/AudioSystem;" and
// continues, but any field of AudioSystem that the clinit failed to
// initialize remains in a half-set state (zero-init defaults plus
// whatever ran before the throw).  PHASE F's discovery probe
// (AudioSystem.getMasterMute()) trips on this:
//
//     DISCOVER-FAIL: PHASE F: AudioSystem chain threw
//       java.lang.reflect.InvocationTargetException: null
//     cause[1]: UnsatisfiedLinkError on getMasterMute
//
// Strategy: STUBS ONLY (M5-PRE scope is clinit unblock).  Real audio
// routing is M5's job (the westlake-audio-daemon), which will wire
// these natives into the libaudioclient / AudioFlinger IPC pipeline.
// For now every native returns a sensible default:
//   - native_getMaxChannelCount       -> 8     (AOSP common default)
//   - native_getMaxSampleRate         -> 192000
//   - native_getMinSampleRate         -> 4000
//   - getMaster* (getMasterMute etc.) -> false / 1.0f / 0
//   - getStreamVolume / Index methods -> 5 or 0.5 (mid-range)
//   - getDeviceConnectionState        -> 0     (unconnected)
//   - all "set" methods               -> 0     (SUCCESS)
//   - native_register_*_callback      -> void  (no-op)
//   - other I-returning queries       -> 0
//   - boolean queries                 -> false
//   - jobject getters                 -> null (callers null-check)
//   - jstring getters                 -> ""
//
// AOSP `AudioSystem.SUCCESS == 0`, so every "set"/"clear" returning 0
// means "operation succeeded" from the caller's perspective.
//
// Pattern mirrors art-latest/stubs/assetmanager_jni_stub.cc (M4-PRE7,
// 56 natives) and art-latest/stubs/messagequeue_jni_stub.cc (M4-PRE4,
// 6 natives):
//   - Java_android_media_AudioSystem_<methodName>* functions
//   - JNI_OnLoad_audiosystem_with_cl registers all natives at load time
//   - Chained from binder_jni_stub.cc:JNI_OnLoad_binder_with_cl
//
// Counts from framework.jar (Android 16, OnePlus 6 phone framework):
//   105 native methods declared on android.media.AudioSystem.
//
// Author: M5-PRE agent  *  Date: 2026-05-12

#include <jni.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <android/log.h>

#define TAG "WLK-audiosys-jni"
#define LOGI(fmt, ...) \
    do { \
        __android_log_print(ANDROID_LOG_INFO, TAG, fmt, ##__VA_ARGS__); \
        fprintf(stderr, "[" TAG "] " fmt "\n", ##__VA_ARGS__); \
    } while (0)
#define LOGE(fmt, ...) \
    do { \
        __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##__VA_ARGS__); \
        fprintf(stderr, "[" TAG " err] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

// ============================================================================
// JNI entrypoints — Java_android_media_AudioSystem_*
// ============================================================================
// Every native is a one-line stub returning a safe default.  When the M5
// audio daemon lands, swap these out for real IAudio* IPC.  For M5-PRE,
// the goal is purely to make AudioSystem.<clinit> succeed and any subset
// of AudioSystem static native calls return non-throwing defaults.
// ============================================================================

extern "C" {

// ---- int-returning natives — return 0 (SUCCESS / unconnected / zero-count) ----

#define STUB_INT_0(JNAME) \
    JNIEXPORT jint JNICALL JNAME(JNIEnv*, jclass, ...) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_addDevicesRoleForCapturePreset(
        JNIEnv*, jclass, jint, jint, jintArray, jobjectArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_checkAudioFlinger(JNIEnv*, jclass) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_clearDevicesRoleForCapturePreset(
        JNIEnv*, jclass, jint, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_clearDevicesRoleForStrategy(
        JNIEnv*, jclass, jint, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_clearPreferredMixerAttributes(
        JNIEnv*, jclass, jobject /*attrs*/, jint, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_createAudioPatch(
        JNIEnv*, jclass, jobjectArray, jobjectArray, jobjectArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getAudioHwSyncForSession(
        JNIEnv*, jclass, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getDeviceConnectionState(
        JNIEnv*, jclass, jint, jstring) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getDevicesForAttributes(
        JNIEnv*, jclass, jobject /*attrs*/, jobjectArray /*devices*/, jboolean) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getDevicesForRoleAndCapturePreset(
        JNIEnv*, jclass, jint, jint, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getDevicesForRoleAndStrategy(
        JNIEnv*, jclass, jint, jint, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getDirectPlaybackSupport(
        JNIEnv*, jclass, jobject /*format*/, jobject /*attrs*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getDirectProfilesForAttributes(
        JNIEnv*, jclass, jobject /*attrs*/, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getForceUse(JNIEnv*, jclass, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getHwOffloadFormatsSupportedForBluetoothMedia(
        JNIEnv*, jclass, jint, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getMaxVolumeIndexForAttributes(
        JNIEnv*, jclass, jobject /*attrs*/) { return 25; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getMicrophones(JNIEnv*, jclass, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getMinVolumeIndexForAttributes(
        JNIEnv*, jclass, jobject /*attrs*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getOutputLatency(JNIEnv*, jclass, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getPreferredMixerAttributes(
        JNIEnv*, jclass, jobject /*attrs*/, jint, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getPrimaryOutputFrameCount(JNIEnv*, jclass) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getPrimaryOutputSamplingRate(JNIEnv*, jclass) { return 48000; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getRegisteredPolicyMixes(
        JNIEnv*, jclass, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getReportedSurroundFormats(
        JNIEnv*, jclass, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getStreamVolumeIndex(
        JNIEnv*, jclass, jint, jint) { return 5; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getSupportedDeviceTypes(
        JNIEnv*, jclass, jint, jobject /*outArray*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getSupportedMixerAttributes(
        JNIEnv*, jclass, jint, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getSurroundFormats(
        JNIEnv*, jclass, jobject /*map*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_getVolumeIndexForAttributes(
        JNIEnv*, jclass, jobject /*attrs*/, jint) { return 5; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_handleDeviceConfigChange(
        JNIEnv*, jclass, jint, jstring, jstring, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_initStreamVolume(
        JNIEnv*, jclass, jint, jint, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_listAudioPatches(
        JNIEnv*, jclass, jobject /*list*/, jintArray /*generation*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_listAudioPorts(
        JNIEnv*, jclass, jobject /*list*/, jintArray /*generation*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_muteMicrophone(JNIEnv*, jclass, jboolean) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_native_1getMaxChannelCount(JNIEnv*, jclass) {
    // AOSP default.  Used by AudioFormat.CHANNEL_OUT_5POINT1 etc. validation.
    return 8;
}

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_native_1getMaxSampleRate(JNIEnv*, jclass) {
    return 192000;
}

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_native_1getMinSampleRate(JNIEnv*, jclass) {
    return 4000;
}

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_native_1get_1offload_1support(
        JNIEnv*, jclass, jint, jint, jint, jint, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_newAudioPlayerId(JNIEnv*, jclass) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_newAudioRecorderId(JNIEnv*, jclass) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_newAudioSessionId(JNIEnv*, jclass) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_registerPolicyMixes(
        JNIEnv*, jclass, jobject /*list*/, jboolean) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_releaseAudioPatch(
        JNIEnv*, jclass, jobject /*patch*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_removeDevicesRoleForCapturePreset(
        JNIEnv*, jclass, jint, jint, jintArray, jobjectArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_removeDevicesRoleForStrategy(
        JNIEnv*, jclass, jint, jint, jintArray, jobjectArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_removeUidDeviceAffinities(
        JNIEnv*, jclass, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_removeUserIdDeviceAffinities(
        JNIEnv*, jclass, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setA11yServicesUids(
        JNIEnv*, jclass, jintArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setActiveAssistantServicesUids(
        JNIEnv*, jclass, jintArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setAllowedCapturePolicy(
        JNIEnv*, jclass, jint, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setAssistantServicesUids(
        JNIEnv*, jclass, jintArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setAudioHalPids(
        JNIEnv*, jclass, jintArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setAudioPortConfig(
        JNIEnv*, jclass, jobject /*cfg*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setBluetoothVariableLatencyEnabled(
        JNIEnv*, jclass, jboolean) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setCurrentImeUid(JNIEnv*, jclass, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setDeviceAbsoluteVolumeEnabled(
        JNIEnv*, jclass, jint, jstring, jboolean, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setDeviceConnectionState(
        JNIEnv*, jclass, jint, jobject /*parcel*/, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setDevicesRoleForCapturePreset(
        JNIEnv*, jclass, jint, jint, jintArray, jobjectArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setDevicesRoleForStrategy(
        JNIEnv*, jclass, jint, jint, jintArray, jobjectArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setForceUse(JNIEnv*, jclass, jint, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setLowRamDevice(
        JNIEnv*, jclass, jboolean, jlong) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setMasterBalance(JNIEnv*, jclass, jfloat) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setMasterMono(JNIEnv*, jclass, jboolean) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setMasterMute(JNIEnv*, jclass, jboolean) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setMasterVolume(JNIEnv*, jclass, jfloat) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setParameters(JNIEnv*, jclass, jstring) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setPhoneState(JNIEnv*, jclass, jint, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setPreferredMixerAttributes(
        JNIEnv*, jclass, jobject /*attrs*/, jint, jint, jobject /*mixerAttrs*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setRttEnabled(JNIEnv*, jclass, jboolean) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setStreamVolumeIndex(
        JNIEnv*, jclass, jint, jint, jboolean, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setSupportedSystemUsages(
        JNIEnv*, jclass, jintArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setSurroundFormatEnabled(
        JNIEnv*, jclass, jint, jboolean) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setUidDeviceAffinities(
        JNIEnv*, jclass, jint, jintArray, jobjectArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setUserIdDeviceAffinities(
        JNIEnv*, jclass, jint, jintArray, jobjectArray) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setVibratorInfos(
        JNIEnv*, jclass, jobject /*list*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_setVolumeIndexForAttributes(
        JNIEnv*, jclass, jobject /*attrs*/, jint, jboolean, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_startAudioSource(
        JNIEnv*, jclass, jobject /*cfg*/, jobject /*attrs*/) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_stopAudioSource(JNIEnv*, jclass, jint) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_systemReady(JNIEnv*, jclass) { return 0; }

JNIEXPORT jint JNICALL
Java_android_media_AudioSystem_updatePolicyMixes(
        JNIEnv*, jclass, jobjectArray, jobjectArray) { return 0; }

// ---- float-returning natives ----

JNIEXPORT jfloat JNICALL
Java_android_media_AudioSystem_getMasterBalance(JNIEnv*, jclass) {
    return 0.0f;  // centered
}

JNIEXPORT jfloat JNICALL
Java_android_media_AudioSystem_getMasterVolume(JNIEnv*, jclass) {
    return 1.0f;  // unity
}

JNIEXPORT jfloat JNICALL
Java_android_media_AudioSystem_getStreamVolumeDB(
        JNIEnv*, jclass, jint, jint, jint) {
    return 0.0f;  // no attenuation
}

// ---- long-returning natives ----

JNIEXPORT jlong JNICALL
Java_android_media_AudioSystem_listenForSystemPropertyChange(
        JNIEnv*, jclass, jstring, jobject /*runnable*/) {
    // Return a non-zero opaque "token" so callers that null-check pass.
    return 0xA1A5A5A1L;
}

// ---- boolean-returning natives ----

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_canBeSpatialized(
        JNIEnv*, jclass, jobject /*attrs*/, jobject /*format*/, jobjectArray /*devices*/) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_getMasterMono(JNIEnv*, jclass) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_getMasterMute(JNIEnv*, jclass) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_isBluetoothVariableLatencyEnabled(JNIEnv*, jclass) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_isCallScreeningModeSupported(JNIEnv*, jclass) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_isHapticPlaybackSupported(JNIEnv*, jclass) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_isMicrophoneMuted(JNIEnv*, jclass) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_isSourceActive(JNIEnv*, jclass, jint) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_isStreamActive(JNIEnv*, jclass, jint, jint) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_isStreamActiveRemotely(JNIEnv*, jclass, jint, jint) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_isUltrasoundSupported(JNIEnv*, jclass) {
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_android_media_AudioSystem_supportsBluetoothVariableLatency(JNIEnv*, jclass) {
    return JNI_FALSE;
}

// ---- void-returning natives ----

JNIEXPORT void JNICALL
Java_android_media_AudioSystem_native_1register_1dynamic_1policy_1callback(
        JNIEnv*, jclass) {}

JNIEXPORT void JNICALL
Java_android_media_AudioSystem_native_1register_1recording_1callback(
        JNIEnv*, jclass) {}

JNIEXPORT void JNICALL
Java_android_media_AudioSystem_native_1register_1routing_1callback(
        JNIEnv*, jclass) {}

JNIEXPORT void JNICALL
Java_android_media_AudioSystem_native_1register_1vol_1range_1init_1req_1callback(
        JNIEnv*, jclass) {}

JNIEXPORT void JNICALL
Java_android_media_AudioSystem_setAudioFlingerBinder(
        JNIEnv*, jclass, jobject /*binder*/) {}

JNIEXPORT void JNICALL
Java_android_media_AudioSystem_triggerSystemPropertyUpdate(
        JNIEnv*, jclass, jlong) {}

// ---- jstring-returning natives ----

JNIEXPORT jstring JNICALL
Java_android_media_AudioSystem_getParameters(
        JNIEnv* env, jclass, jstring /*keys*/) {
    return env->NewStringUTF("");
}

// ---- jobject-returning natives ----

JNIEXPORT jobject JNICALL
Java_android_media_AudioSystem_nativeGetSoundDose(
        JNIEnv*, jclass, jobject /*callback*/) {
    // Returning null is safe — AOSP callers test for null before unwrapping.
    return nullptr;
}

JNIEXPORT jobject JNICALL
Java_android_media_AudioSystem_nativeGetSpatializer(
        JNIEnv*, jclass, jobject /*callback*/) {
    return nullptr;
}

}  // extern "C"

// ============================================================================
// JNI_OnLoad_audiosystem_with_cl — explicit RegisterNatives entry point.
//
// Called from binder_jni_stub.cc:JNI_OnLoad_binder_with_cl which is invoked
// from openjdk_stub.c:Runtime_nativeLoad when ServiceManager.<clinit>
// calls System.loadLibrary("android_runtime_stub"). The classLoader is the
// caller's classloader so we can look up framework.jar's AudioSystem class
// even though it's on the bootclasspath.
// ============================================================================

// Helper: copy of binder_jni_stub's loadClassByName.
static jclass loadAudioSystemClassByName(JNIEnv* env, jobject classLoader,
                                          const char* dotted_name) {
    if (classLoader == nullptr) {
        return env->FindClass(dotted_name);
    }
    jclass clCls = env->GetObjectClass(classLoader);
    if (clCls == nullptr) {
        env->ExceptionClear();
        return env->FindClass(dotted_name);
    }
    jmethodID mid = env->GetMethodID(clCls, "loadClass",
                                      "(Ljava/lang/String;)Ljava/lang/Class;");
    if (mid == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(clCls);
        return env->FindClass(dotted_name);
    }
    std::string dotted = dotted_name;
    for (char& c : dotted) if (c == '/') c = '.';
    jstring js = env->NewStringUTF(dotted.c_str());
    jobject c = env->CallObjectMethod(classLoader, mid, js);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(clCls);
        env->DeleteLocalRef(js);
        return env->FindClass(dotted_name);
    }
    env->DeleteLocalRef(clCls);
    env->DeleteLocalRef(js);
    return reinterpret_cast<jclass>(c);
}

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad_audiosystem_with_cl(JavaVM* vm, jobject classLoader) {
    LOGI("JNI_OnLoad_audiosystem: vm=%p classLoader=%p", vm, classLoader);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        LOGE("JNI_OnLoad_audiosystem: GetEnv failed");
        return JNI_VERSION_1_6;
    }

    jclass cls = loadAudioSystemClassByName(env, classLoader, "android/media/AudioSystem");
    if (cls == nullptr) {
        env->ExceptionClear();
        cls = env->FindClass("android/media/AudioSystem");
    }
    if (cls == nullptr) {
        env->ExceptionClear();
        LOGE("JNI_OnLoad_audiosystem: FindClass(android/media/AudioSystem) failed (CL=%p) — "
             "framework.jar not loaded yet; will be retried on next load",
             classLoader);
        return JNI_VERSION_1_6;
    }

    JNINativeMethod methods[] = {
        // Int-returning (count results, error codes etc.).
        {const_cast<char*>("addDevicesRoleForCapturePreset"),
         const_cast<char*>("(II[I[Ljava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_addDevicesRoleForCapturePreset)},
        {const_cast<char*>("checkAudioFlinger"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_checkAudioFlinger)},
        {const_cast<char*>("clearDevicesRoleForCapturePreset"),
         const_cast<char*>("(II)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_clearDevicesRoleForCapturePreset)},
        {const_cast<char*>("clearDevicesRoleForStrategy"),
         const_cast<char*>("(II)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_clearDevicesRoleForStrategy)},
        {const_cast<char*>("clearPreferredMixerAttributes"),
         const_cast<char*>("(Landroid/media/AudioAttributes;II)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_clearPreferredMixerAttributes)},
        {const_cast<char*>("createAudioPatch"),
         const_cast<char*>("([Landroid/media/AudioPatch;[Landroid/media/AudioPortConfig;[Landroid/media/AudioPortConfig;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_createAudioPatch)},
        {const_cast<char*>("getAudioHwSyncForSession"),
         const_cast<char*>("(I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getAudioHwSyncForSession)},
        {const_cast<char*>("getDeviceConnectionState"),
         const_cast<char*>("(ILjava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getDeviceConnectionState)},
        {const_cast<char*>("getDevicesForAttributes"),
         const_cast<char*>("(Landroid/media/AudioAttributes;[Landroid/media/AudioDeviceAttributes;Z)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getDevicesForAttributes)},
        {const_cast<char*>("getDevicesForRoleAndCapturePreset"),
         const_cast<char*>("(IILjava/util/List;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getDevicesForRoleAndCapturePreset)},
        {const_cast<char*>("getDevicesForRoleAndStrategy"),
         const_cast<char*>("(IILjava/util/List;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getDevicesForRoleAndStrategy)},
        {const_cast<char*>("getDirectPlaybackSupport"),
         const_cast<char*>("(Landroid/media/AudioFormat;Landroid/media/AudioAttributes;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getDirectPlaybackSupport)},
        {const_cast<char*>("getDirectProfilesForAttributes"),
         const_cast<char*>("(Landroid/media/AudioAttributes;Ljava/util/ArrayList;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getDirectProfilesForAttributes)},
        {const_cast<char*>("getForceUse"),
         const_cast<char*>("(I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getForceUse)},
        {const_cast<char*>("getHwOffloadFormatsSupportedForBluetoothMedia"),
         const_cast<char*>("(ILjava/util/ArrayList;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getHwOffloadFormatsSupportedForBluetoothMedia)},
        {const_cast<char*>("getMaxVolumeIndexForAttributes"),
         const_cast<char*>("(Landroid/media/AudioAttributes;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getMaxVolumeIndexForAttributes)},
        {const_cast<char*>("getMicrophones"),
         const_cast<char*>("(Ljava/util/ArrayList;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getMicrophones)},
        {const_cast<char*>("getMinVolumeIndexForAttributes"),
         const_cast<char*>("(Landroid/media/AudioAttributes;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getMinVolumeIndexForAttributes)},
        {const_cast<char*>("getOutputLatency"),
         const_cast<char*>("(I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getOutputLatency)},
        {const_cast<char*>("getPreferredMixerAttributes"),
         const_cast<char*>("(Landroid/media/AudioAttributes;ILjava/util/List;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getPreferredMixerAttributes)},
        {const_cast<char*>("getPrimaryOutputFrameCount"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getPrimaryOutputFrameCount)},
        {const_cast<char*>("getPrimaryOutputSamplingRate"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getPrimaryOutputSamplingRate)},
        {const_cast<char*>("getRegisteredPolicyMixes"),
         const_cast<char*>("(Ljava/util/List;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getRegisteredPolicyMixes)},
        {const_cast<char*>("getReportedSurroundFormats"),
         const_cast<char*>("(Ljava/util/ArrayList;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getReportedSurroundFormats)},
        {const_cast<char*>("getStreamVolumeIndex"),
         const_cast<char*>("(II)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getStreamVolumeIndex)},
        {const_cast<char*>("getSupportedDeviceTypes"),
         const_cast<char*>("(ILandroid/util/IntArray;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getSupportedDeviceTypes)},
        {const_cast<char*>("getSupportedMixerAttributes"),
         const_cast<char*>("(ILjava/util/List;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getSupportedMixerAttributes)},
        {const_cast<char*>("getSurroundFormats"),
         const_cast<char*>("(Ljava/util/Map;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getSurroundFormats)},
        {const_cast<char*>("getVolumeIndexForAttributes"),
         const_cast<char*>("(Landroid/media/AudioAttributes;I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getVolumeIndexForAttributes)},
        {const_cast<char*>("handleDeviceConfigChange"),
         const_cast<char*>("(ILjava/lang/String;Ljava/lang/String;I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_handleDeviceConfigChange)},
        {const_cast<char*>("initStreamVolume"),
         const_cast<char*>("(III)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_initStreamVolume)},
        {const_cast<char*>("listAudioPatches"),
         const_cast<char*>("(Ljava/util/ArrayList;[I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_listAudioPatches)},
        {const_cast<char*>("listAudioPorts"),
         const_cast<char*>("(Ljava/util/ArrayList;[I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_listAudioPorts)},
        {const_cast<char*>("muteMicrophone"),
         const_cast<char*>("(Z)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_muteMicrophone)},
        {const_cast<char*>("native_getMaxChannelCount"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_native_1getMaxChannelCount)},
        {const_cast<char*>("native_getMaxSampleRate"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_native_1getMaxSampleRate)},
        {const_cast<char*>("native_getMinSampleRate"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_native_1getMinSampleRate)},
        {const_cast<char*>("native_get_offload_support"),
         const_cast<char*>("(IIIII)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_native_1get_1offload_1support)},
        {const_cast<char*>("newAudioPlayerId"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_newAudioPlayerId)},
        {const_cast<char*>("newAudioRecorderId"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_newAudioRecorderId)},
        {const_cast<char*>("newAudioSessionId"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_newAudioSessionId)},
        {const_cast<char*>("registerPolicyMixes"),
         const_cast<char*>("(Ljava/util/ArrayList;Z)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_registerPolicyMixes)},
        {const_cast<char*>("releaseAudioPatch"),
         const_cast<char*>("(Landroid/media/AudioPatch;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_releaseAudioPatch)},
        {const_cast<char*>("removeDevicesRoleForCapturePreset"),
         const_cast<char*>("(II[I[Ljava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_removeDevicesRoleForCapturePreset)},
        {const_cast<char*>("removeDevicesRoleForStrategy"),
         const_cast<char*>("(II[I[Ljava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_removeDevicesRoleForStrategy)},
        {const_cast<char*>("removeUidDeviceAffinities"),
         const_cast<char*>("(I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_removeUidDeviceAffinities)},
        {const_cast<char*>("removeUserIdDeviceAffinities"),
         const_cast<char*>("(I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_removeUserIdDeviceAffinities)},
        {const_cast<char*>("setA11yServicesUids"),
         const_cast<char*>("([I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setA11yServicesUids)},
        {const_cast<char*>("setActiveAssistantServicesUids"),
         const_cast<char*>("([I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setActiveAssistantServicesUids)},
        {const_cast<char*>("setAllowedCapturePolicy"),
         const_cast<char*>("(II)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setAllowedCapturePolicy)},
        {const_cast<char*>("setAssistantServicesUids"),
         const_cast<char*>("([I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setAssistantServicesUids)},
        {const_cast<char*>("setAudioHalPids"),
         const_cast<char*>("([I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setAudioHalPids)},
        {const_cast<char*>("setAudioPortConfig"),
         const_cast<char*>("(Landroid/media/AudioPortConfig;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setAudioPortConfig)},
        {const_cast<char*>("setBluetoothVariableLatencyEnabled"),
         const_cast<char*>("(Z)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setBluetoothVariableLatencyEnabled)},
        {const_cast<char*>("setCurrentImeUid"),
         const_cast<char*>("(I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setCurrentImeUid)},
        {const_cast<char*>("setDeviceAbsoluteVolumeEnabled"),
         const_cast<char*>("(ILjava/lang/String;ZI)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setDeviceAbsoluteVolumeEnabled)},
        {const_cast<char*>("setDeviceConnectionState"),
         const_cast<char*>("(ILandroid/os/Parcel;I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setDeviceConnectionState)},
        {const_cast<char*>("setDevicesRoleForCapturePreset"),
         const_cast<char*>("(II[I[Ljava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setDevicesRoleForCapturePreset)},
        {const_cast<char*>("setDevicesRoleForStrategy"),
         const_cast<char*>("(II[I[Ljava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setDevicesRoleForStrategy)},
        {const_cast<char*>("setForceUse"),
         const_cast<char*>("(II)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setForceUse)},
        {const_cast<char*>("setLowRamDevice"),
         const_cast<char*>("(ZJ)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setLowRamDevice)},
        {const_cast<char*>("setMasterBalance"),
         const_cast<char*>("(F)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setMasterBalance)},
        {const_cast<char*>("setMasterMono"),
         const_cast<char*>("(Z)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setMasterMono)},
        {const_cast<char*>("setMasterMute"),
         const_cast<char*>("(Z)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setMasterMute)},
        {const_cast<char*>("setMasterVolume"),
         const_cast<char*>("(F)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setMasterVolume)},
        {const_cast<char*>("setParameters"),
         const_cast<char*>("(Ljava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setParameters)},
        {const_cast<char*>("setPhoneState"),
         const_cast<char*>("(II)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setPhoneState)},
        {const_cast<char*>("setPreferredMixerAttributes"),
         const_cast<char*>("(Landroid/media/AudioAttributes;IILandroid/media/AudioMixerAttributes;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setPreferredMixerAttributes)},
        {const_cast<char*>("setRttEnabled"),
         const_cast<char*>("(Z)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setRttEnabled)},
        {const_cast<char*>("setStreamVolumeIndex"),
         const_cast<char*>("(IIZI)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setStreamVolumeIndex)},
        {const_cast<char*>("setSupportedSystemUsages"),
         const_cast<char*>("([I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setSupportedSystemUsages)},
        {const_cast<char*>("setSurroundFormatEnabled"),
         const_cast<char*>("(IZ)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setSurroundFormatEnabled)},
        {const_cast<char*>("setUidDeviceAffinities"),
         const_cast<char*>("(I[I[Ljava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setUidDeviceAffinities)},
        {const_cast<char*>("setUserIdDeviceAffinities"),
         const_cast<char*>("(I[I[Ljava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setUserIdDeviceAffinities)},
        {const_cast<char*>("setVibratorInfos"),
         const_cast<char*>("(Ljava/util/List;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setVibratorInfos)},
        {const_cast<char*>("setVolumeIndexForAttributes"),
         const_cast<char*>("(Landroid/media/AudioAttributes;IZI)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setVolumeIndexForAttributes)},
        {const_cast<char*>("startAudioSource"),
         const_cast<char*>("(Landroid/media/AudioPortConfig;Landroid/media/AudioAttributes;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_startAudioSource)},
        {const_cast<char*>("stopAudioSource"),
         const_cast<char*>("(I)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_stopAudioSource)},
        {const_cast<char*>("systemReady"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_systemReady)},
        {const_cast<char*>("updatePolicyMixes"),
         const_cast<char*>("([Landroid/media/audiopolicy/AudioMix;[Landroid/media/audiopolicy/AudioMixingRule;)I"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_updatePolicyMixes)},
        // Float-returning.
        {const_cast<char*>("getMasterBalance"),
         const_cast<char*>("()F"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getMasterBalance)},
        {const_cast<char*>("getMasterVolume"),
         const_cast<char*>("()F"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getMasterVolume)},
        {const_cast<char*>("getStreamVolumeDB"),
         const_cast<char*>("(III)F"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getStreamVolumeDB)},
        // Long-returning.
        {const_cast<char*>("listenForSystemPropertyChange"),
         const_cast<char*>("(Ljava/lang/String;Ljava/lang/Runnable;)J"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_listenForSystemPropertyChange)},
        // Boolean-returning.
        {const_cast<char*>("canBeSpatialized"),
         const_cast<char*>("(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;[Landroid/media/AudioDeviceAttributes;)Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_canBeSpatialized)},
        {const_cast<char*>("getMasterMono"),
         const_cast<char*>("()Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getMasterMono)},
        {const_cast<char*>("getMasterMute"),
         const_cast<char*>("()Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getMasterMute)},
        {const_cast<char*>("isBluetoothVariableLatencyEnabled"),
         const_cast<char*>("()Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_isBluetoothVariableLatencyEnabled)},
        {const_cast<char*>("isCallScreeningModeSupported"),
         const_cast<char*>("()Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_isCallScreeningModeSupported)},
        {const_cast<char*>("isHapticPlaybackSupported"),
         const_cast<char*>("()Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_isHapticPlaybackSupported)},
        {const_cast<char*>("isMicrophoneMuted"),
         const_cast<char*>("()Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_isMicrophoneMuted)},
        {const_cast<char*>("isSourceActive"),
         const_cast<char*>("(I)Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_isSourceActive)},
        {const_cast<char*>("isStreamActive"),
         const_cast<char*>("(II)Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_isStreamActive)},
        {const_cast<char*>("isStreamActiveRemotely"),
         const_cast<char*>("(II)Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_isStreamActiveRemotely)},
        {const_cast<char*>("isUltrasoundSupported"),
         const_cast<char*>("()Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_isUltrasoundSupported)},
        {const_cast<char*>("supportsBluetoothVariableLatency"),
         const_cast<char*>("()Z"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_supportsBluetoothVariableLatency)},
        // Void-returning.
        {const_cast<char*>("native_register_dynamic_policy_callback"),
         const_cast<char*>("()V"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_native_1register_1dynamic_1policy_1callback)},
        {const_cast<char*>("native_register_recording_callback"),
         const_cast<char*>("()V"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_native_1register_1recording_1callback)},
        {const_cast<char*>("native_register_routing_callback"),
         const_cast<char*>("()V"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_native_1register_1routing_1callback)},
        {const_cast<char*>("native_register_vol_range_init_req_callback"),
         const_cast<char*>("()V"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_native_1register_1vol_1range_1init_1req_1callback)},
        {const_cast<char*>("setAudioFlingerBinder"),
         const_cast<char*>("(Landroid/os/IBinder;)V"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_setAudioFlingerBinder)},
        {const_cast<char*>("triggerSystemPropertyUpdate"),
         const_cast<char*>("(J)V"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_triggerSystemPropertyUpdate)},
        // String-returning.
        {const_cast<char*>("getParameters"),
         const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_getParameters)},
        // Object-returning.
        {const_cast<char*>("nativeGetSoundDose"),
         const_cast<char*>("(Landroid/media/ISoundDoseCallback;)Landroid/os/IBinder;"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_nativeGetSoundDose)},
        {const_cast<char*>("nativeGetSpatializer"),
         const_cast<char*>("(Landroid/media/INativeSpatializerCallback;)Landroid/os/IBinder;"),
         reinterpret_cast<void*>(Java_android_media_AudioSystem_nativeGetSpatializer)},
    };
    int n = sizeof(methods) / sizeof(methods[0]);
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
            ++ok;
        } else {
            env->ExceptionClear();
            LOGE("JNI_OnLoad_audiosystem: RegisterNatives(android.media.AudioSystem.%s%s) failed — "
                 "framework.jar AudioSystem may not declare this native (older API level?)",
                 methods[i].name, methods[i].signature);
        }
    }
    LOGI("JNI_OnLoad_audiosystem: android.media.AudioSystem natives: %d/%d", ok, n);
    env->DeleteLocalRef(cls);
    return JNI_VERSION_1_6;
}

// Legacy classloader-less entry.
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad_audiosystem(JavaVM* vm, void* /*reserved*/) {
    return JNI_OnLoad_audiosystem_with_cl(vm, nullptr);
}
