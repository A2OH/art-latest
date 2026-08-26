// SPDX-License-Identifier: Apache-2.0
//
// art-latest/stubs/assetmanager_jni_stub.cc — Westlake M4-PRE7
//
// Statically linked JNI stubs for android.content.res.AssetManager's
// native methods. Purpose:
//   Unblock ResourcesImpl.<clinit> -> sThemeRegistry init which calls
//   AssetManager.getThemeFreeFunction() -> nativeGetThemeFreeFunction().
//   The clinit currently fails with UnsatisfiedLinkError, then ART
//   tolerates the failure but Resources construction NPEs on any field
//   that ResourcesImpl.<clinit> failed to initialize. By providing safe
//   sentinel implementations of all 56 AssetManager natives we let
//   ResourcesImpl.<clinit> succeed so WestlakeResources.createSafe()
//   returns a usable Resources object.
//
// Strategy: STUBS ONLY. No real asset management. The goal is to make
// the clinit and constructor calls succeed; any actual asset lookup
// will surface as a discovery datapoint (loud failure on dereferencing
// the sentinel handle).
//
// Pattern mirrors messagequeue_jni_stub.cc:
//   - Java_android_content_res_AssetManager_<methodName>* functions
//   - JNI_OnLoad_assetmanager_with_cl registers all natives at load time
//   - Chained from binder_jni_stub.cc:JNI_OnLoad_binder_with_cl
//
// Sentinel handle strategy:
//   nativeCreate -> returns a heap-allocated small struct pointer.
//   Future natives that take a "jlong handle" simply ignore the value.
//   Real AOSP code stores android::AssetManager2* in the handle; we
//   never let real AOSP code see this handle so the cast is safe.
//
// nativeGetThemeFreeFunction returns a pointer to a no-op C function
// matching the NativeAllocationRegistry "free function" signature:
//   void free_function(void* native_ptr)
// — used by registerNativeAllocation when the Theme is GC'd.
//
// Author: M4-PRE7 agent  *  Date: 2026-05-12

#include <jni.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <android/log.h>

#define TAG "WLK-am-jni"
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

namespace {

// Sentinel "AssetManager" structure. Just a marker so handle != 0.
// Real AOSP code stores android::AssetManager2* here; we never call
// into that code so the layout doesn't matter.
struct StubAssetManager {
    uint32_t magic;  // 0x57414D32 ("WAM2")
    char     name[32];
};

// Sentinel "Theme" structure. Same idea — handle marker.
struct StubTheme {
    uint32_t magic;  // 0x57544845 ("WTHE")
    int32_t  ref;
};

// no-op free function returned by nativeGetThemeFreeFunction.
// Signature: void(*)(void* native_ptr) per NativeAllocationRegistry.
extern "C" void westlake_theme_free(void* p) {
    if (p != nullptr) {
        StubTheme* t = reinterpret_cast<StubTheme*>(p);
        if (t->magic == 0x57544845) {
            free(t);
        }
        // Otherwise it's a real AOSP Theme — leave alone (no-op).
    }
}

// no-op free function for asset manager handles.
extern "C" void westlake_assetmanager_free(void* p) {
    if (p != nullptr) {
        StubAssetManager* m = reinterpret_cast<StubAssetManager*>(p);
        if (m->magic == 0x57414D32) {
            free(m);
        }
    }
}

}  // namespace

// ============================================================================
// JNI entrypoints — Java_android_content_res_AssetManager_*
//
// All 56 native methods declared in framework.jar's
// android.content.res.AssetManager class are stubbed here. Each one
// returns a safe sentinel:
//   - long handle methods (nativeCreate, nativeThemeCreate)  -> ptr to stub
//   - long getter methods (nativeGetThemeFreeFunction)       -> function ptr
//   - long size methods (nativeAssetGet*Length, etc.)        -> 0
//   - int/short methods                                       -> 0
//   - boolean methods                                         -> false
//   - String methods                                          -> empty string
//   - Array methods                                           -> empty array
//   - void methods                                            -> no-op
// ============================================================================

// ---- Static debug/global methods (PUBLIC STATIC NATIVE) ----

extern "C" JNIEXPORT jstring JNICALL
Java_android_content_res_AssetManager_getAssetAllocations(JNIEnv* env, jclass) {
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_getGlobalAssetCount(JNIEnv*, jclass) {
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_getGlobalAssetManagerCount(JNIEnv*, jclass) {
    return 0;
}

// ---- nativeCreate / nativeDestroy ----

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeCreate(JNIEnv*, jclass) {
    StubAssetManager* m = static_cast<StubAssetManager*>(calloc(1, sizeof(StubAssetManager)));
    if (m == nullptr) return 0;
    m->magic = 0x57414D32;
    strncpy(m->name, "westlake-stub", sizeof(m->name) - 1);
    LOGI("nativeCreate -> %p", m);
    return reinterpret_cast<jlong>(m);
}

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeDestroy(JNIEnv*, jclass, jlong ptr) {
    if (ptr == 0) return;
    StubAssetManager* m = reinterpret_cast<StubAssetManager*>(ptr);
    if (m->magic == 0x57414D32) {
        free(m);
    }
}

// ---- Theme management ----

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeThemeCreate(JNIEnv*, jclass, jlong /*amHandle*/) {
    StubTheme* t = static_cast<StubTheme*>(calloc(1, sizeof(StubTheme)));
    if (t == nullptr) return 0;
    t->magic = 0x57544845;
    t->ref = 1;
    return reinterpret_cast<jlong>(t);
}

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeGetThemeFreeFunction(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&westlake_theme_free);
}

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeThemeApplyStyle(
        JNIEnv*, jclass, jlong /*amHandle*/, jlong /*themeHandle*/,
        jint /*resId*/, jboolean /*force*/) {
}

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeThemeCopy(
        JNIEnv*, jclass, jlong /*dstAmHandle*/, jlong /*dstHandle*/,
        jlong /*srcAmHandle*/, jlong /*srcHandle*/) {
}

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeThemeDump(
        JNIEnv*, jclass, jlong /*amHandle*/, jlong /*themeHandle*/,
        jint /*priority*/, jstring /*tag*/, jstring /*prefix*/) {
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeThemeGetAttributeValue(
        JNIEnv* env, jclass, jlong /*amHandle*/, jlong /*themeHandle*/,
        jint /*resId*/, jobject outValue, jboolean /*resolveRefs*/) {
    // M4-PRE8: same treatment as nativeGetResourceValue.  Theme attribute
    // resolution returns "found, default 0" so theme.resolveAttribute()
    // callers don't NPE on missing TypedValue fields.
    if (outValue != nullptr) {
        jclass tvCls = env->GetObjectClass(outValue);
        if (tvCls != nullptr) {
            jfieldID typeFid = env->GetFieldID(tvCls, "type", "I");
            jfieldID dataFid = env->GetFieldID(tvCls, "data", "I");
            jfieldID resourceIdFid = env->GetFieldID(tvCls, "resourceId", "I");
            jfieldID assetCookieFid = env->GetFieldID(tvCls, "assetCookie", "I");
            jfieldID densityFid = env->GetFieldID(tvCls, "density", "I");
            if (typeFid != nullptr) {
                env->SetIntField(outValue, typeFid, 0x12);   // TYPE_INT_BOOLEAN
            }
            if (dataFid != nullptr) {
                env->SetIntField(outValue, dataFid, 0);
            }
            if (resourceIdFid != nullptr) {
                env->SetIntField(outValue, resourceIdFid, 0);
            }
            if (assetCookieFid != nullptr) {
                env->SetIntField(outValue, assetCookieFid, 1);
            }
            if (densityFid != nullptr) {
                env->SetIntField(outValue, densityFid, 0);
            }
            env->ExceptionClear();
            env->DeleteLocalRef(tvCls);
        } else {
            env->ExceptionClear();
        }
    }
    return 1;
}

// nativeThemeGetChangingConfigurations: (J)I, STATIC NATIVE.
extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeThemeGetChangingConfigurations(
        JNIEnv*, jclass, jlong /*themeHandle*/) {
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeThemeRebase(
        JNIEnv*, jclass, jlong /*amHandle*/, jlong /*themeHandle*/,
        jintArray /*styleIds*/, jbooleanArray /*force*/, jint /*styleSize*/) {
}

// ---- Asset I/O ----

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeAssetDestroy(
        JNIEnv*, jclass, jlong /*assetPtr*/) {
}

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeAssetGetLength(
        JNIEnv*, jclass, jlong /*assetPtr*/) {
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeAssetGetRemainingLength(
        JNIEnv*, jclass, jlong /*assetPtr*/) {
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeAssetRead(
        JNIEnv*, jclass, jlong /*assetPtr*/, jbyteArray /*buffer*/,
        jint /*offset*/, jint /*length*/) {
    return -1;  // EOF
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeAssetReadChar(
        JNIEnv*, jclass, jlong /*assetPtr*/) {
    return -1;  // EOF
}

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeAssetSeek(
        JNIEnv*, jclass, jlong /*assetPtr*/, jlong /*offset*/, jint /*whence*/) {
    return -1;
}

// ---- nativeApplyStyle / nativeApplyStyleWithArray / attribute resolution ----

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeApplyStyle(
        JNIEnv*, jclass, jlong /*amHandle*/, jlong /*themeToken*/,
        jint /*defStyleAttr*/, jint /*defStyleRes*/, jlong /*xmlParserToken*/,
        jintArray /*attrs*/, jlong /*outValuesAddress*/, jlong /*outIndicesAddress*/) {
}

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeApplyStyleWithArray(
        JNIEnv*, jclass, jlong /*amHandle*/, jlong /*themeToken*/,
        jint /*defStyleAttr*/, jint /*defStyleRes*/, jlong /*xmlParserToken*/,
        jintArray /*attrs*/, jintArray /*outValues*/, jintArray /*outIndices*/) {
}

extern "C" JNIEXPORT jintArray JNICALL
Java_android_content_res_AssetManager_nativeAttributeResolutionStack(
        JNIEnv* env, jclass, jlong /*amHandle*/, jlong /*themeToken*/,
        jint /*xmlStyleRes*/, jint /*defStyleAttr*/, jint /*defStyleRes*/) {
    return env->NewIntArray(0);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_android_content_res_AssetManager_nativeResolveAttrs(
        JNIEnv*, jclass, jlong /*amHandle*/, jlong /*themeToken*/,
        jint /*defStyleAttr*/, jint /*defStyleRes*/,
        jintArray /*inValues*/, jintArray /*attrs*/,
        jintArray /*outValues*/, jintArray /*outIndices*/) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_android_content_res_AssetManager_nativeRetrieveAttributes(
        JNIEnv*, jclass, jlong /*amHandle*/, jlong /*xmlParserToken*/,
        jintArray /*attrs*/, jintArray /*outValues*/, jintArray /*outIndices*/) {
    return JNI_FALSE;
}

// ---- Resource lookup ----

extern "C" JNIEXPORT jboolean JNICALL
Java_android_content_res_AssetManager_nativeContainsAllocatedTable(
        JNIEnv*, jclass, jlong /*amHandle*/) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT jobject JNICALL
Java_android_content_res_AssetManager_nativeGetAssignedPackageIdentifiers(
        JNIEnv* env, jclass, jlong /*amHandle*/, jboolean /*includeOverlays*/,
        jboolean /*includeLoaders*/) {
    // Return an empty SparseArray. Use reflection-free path: just null;
    // AOSP callers handle null gracefully (no entries to iterate).
    jclass spArrayCls = env->FindClass("android/util/SparseArray");
    if (spArrayCls == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(spArrayCls, "<init>", "()V");
    if (ctor == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(spArrayCls);
        return nullptr;
    }
    jobject obj = env->NewObject(spArrayCls, ctor);
    env->DeleteLocalRef(spArrayCls);
    return obj;
}

extern "C" JNIEXPORT jstring JNICALL
Java_android_content_res_AssetManager_nativeGetLastResourceResolution(
        JNIEnv* env, jclass, jlong /*amHandle*/) {
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_android_content_res_AssetManager_nativeGetLocales(
        JNIEnv* env, jclass, jlong /*amHandle*/, jboolean /*excludeSystem*/) {
    jclass strCls = env->FindClass("java/lang/String");
    if (strCls == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jobjectArray arr = env->NewObjectArray(0, strCls, nullptr);
    env->DeleteLocalRef(strCls);
    return arr;
}

extern "C" JNIEXPORT jobject JNICALL
Java_android_content_res_AssetManager_nativeGetOverlayableMap(
        JNIEnv* env, jclass, jlong /*amHandle*/, jstring /*packageName*/) {
    jclass mapCls = env->FindClass("java/util/HashMap");
    if (mapCls == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(mapCls, "<init>", "()V");
    if (ctor == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(mapCls);
        return nullptr;
    }
    jobject obj = env->NewObject(mapCls, ctor);
    env->DeleteLocalRef(mapCls);
    return obj;
}

extern "C" JNIEXPORT jstring JNICALL
Java_android_content_res_AssetManager_nativeGetOverlayablesToString(
        JNIEnv* env, jclass, jlong /*amHandle*/, jstring /*packageName*/) {
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeGetParentThemeIdentifier(
        JNIEnv*, jclass, jlong /*amHandle*/, jint /*resId*/) {
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeGetResourceArray(
        JNIEnv*, jclass, jlong /*amHandle*/, jint /*resId*/, jintArray /*outData*/) {
    return -1;
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeGetResourceArraySize(
        JNIEnv*, jclass, jlong /*amHandle*/, jint /*resId*/) {
    return -1;
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeGetResourceBagValue(
        JNIEnv*, jclass, jlong /*amHandle*/, jint /*resId*/,
        jint /*attrId*/, jobject /*outValue*/) {
    return 0;
}

extern "C" JNIEXPORT jstring JNICALL
Java_android_content_res_AssetManager_nativeGetResourceEntryName(
        JNIEnv* env, jclass, jlong /*amHandle*/, jint /*resId*/) {
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeGetResourceIdentifier(
        JNIEnv*, jclass, jlong /*amHandle*/, jstring /*name*/,
        jstring /*defType*/, jstring /*defPackage*/) {
    return 0;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_android_content_res_AssetManager_nativeGetResourceIntArray(
        JNIEnv* env, jclass, jlong /*amHandle*/, jint /*resId*/) {
    return env->NewIntArray(0);
}

extern "C" JNIEXPORT jstring JNICALL
Java_android_content_res_AssetManager_nativeGetResourceName(
        JNIEnv* env, jclass, jlong /*amHandle*/, jint /*resId*/) {
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jstring JNICALL
Java_android_content_res_AssetManager_nativeGetResourcePackageName(
        JNIEnv* env, jclass, jlong /*amHandle*/, jint /*resId*/) {
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_android_content_res_AssetManager_nativeGetResourceStringArray(
        JNIEnv* env, jclass, jlong /*amHandle*/, jint /*resId*/) {
    jclass strCls = env->FindClass("java/lang/String");
    if (strCls == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jobjectArray arr = env->NewObjectArray(0, strCls, nullptr);
    env->DeleteLocalRef(strCls);
    return arr;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_android_content_res_AssetManager_nativeGetResourceStringArrayInfo(
        JNIEnv* env, jclass, jlong /*amHandle*/, jint /*resId*/) {
    return env->NewIntArray(0);
}

extern "C" JNIEXPORT jstring JNICALL
Java_android_content_res_AssetManager_nativeGetResourceTypeName(
        JNIEnv* env, jclass, jlong /*amHandle*/, jint /*resId*/) {
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jint JNICALL
Java_android_content_res_AssetManager_nativeGetResourceValue(
        JNIEnv* env, jclass, jlong /*amHandle*/, jint /*resId*/,
        jshort /*density*/, jobject outValue, jboolean /*resolveRefs*/) {
    // M4-PRE8: populate outValue with TYPE_INT_BOOLEAN=0x12, data=0 so
    // Resources.getBoolean() returns false instead of throwing
    // NotFoundException.  Window.getDefaultFeatures() calls getBoolean for
    // two com.android.internal.R.bool.config_defaultWindowFeature* values
    // during PhoneWindow.<init>; we don't have those resources, so the
    // safest stub is "found, default to false" rather than "not found".
    //
    // Real AOSP behavior: returns the cookie of the resource package
    // (jint > 0) on success, 0 on not-found.  We return 1 (any non-zero
    // is "found") and populate the minimum fields Resources.getBoolean
    // reads (type + data) to drive the TYPE_FIRST_INT..TYPE_LAST_INT
    // range check (TYPE_INT_BOOLEAN=0x12 lies in [0x10..0x1f]).
    //
    // Resources.getValue() callers that need richer fields (string/etc)
    // will see them at their defaults — surfaced as discovery datapoints
    // if a richer call site is reached.
    if (outValue != nullptr) {
        jclass tvCls = env->GetObjectClass(outValue);
        if (tvCls != nullptr) {
            jfieldID typeFid = env->GetFieldID(tvCls, "type", "I");
            jfieldID dataFid = env->GetFieldID(tvCls, "data", "I");
            jfieldID resourceIdFid = env->GetFieldID(tvCls, "resourceId", "I");
            jfieldID assetCookieFid = env->GetFieldID(tvCls, "assetCookie", "I");
            jfieldID densityFid = env->GetFieldID(tvCls, "density", "I");
            if (typeFid != nullptr) {
                env->SetIntField(outValue, typeFid, 0x12);   // TYPE_INT_BOOLEAN
            }
            if (dataFid != nullptr) {
                env->SetIntField(outValue, dataFid, 0);      // false
            }
            if (resourceIdFid != nullptr) {
                env->SetIntField(outValue, resourceIdFid, 0);
            }
            if (assetCookieFid != nullptr) {
                env->SetIntField(outValue, assetCookieFid, 1);
            }
            if (densityFid != nullptr) {
                env->SetIntField(outValue, densityFid, 0);
            }
            env->ExceptionClear();
            env->DeleteLocalRef(tvCls);
        } else {
            env->ExceptionClear();
        }
    }
    return 1;  // any non-zero == "found"
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_android_content_res_AssetManager_nativeGetSizeAndUiModeConfigurations(
        JNIEnv* env, jclass, jlong /*amHandle*/) {
    jclass cfgCls = env->FindClass("android/content/res/Configuration");
    if (cfgCls == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jobjectArray arr = env->NewObjectArray(0, cfgCls, nullptr);
    env->DeleteLocalRef(cfgCls);
    return arr;
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_android_content_res_AssetManager_nativeGetSizeConfigurations(
        JNIEnv* env, jclass, jlong /*amHandle*/) {
    jclass cfgCls = env->FindClass("android/content/res/Configuration");
    if (cfgCls == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jobjectArray arr = env->NewObjectArray(0, cfgCls, nullptr);
    env->DeleteLocalRef(cfgCls);
    return arr;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_android_content_res_AssetManager_nativeGetStyleAttributes(
        JNIEnv* env, jclass, jlong /*amHandle*/, jint /*resId*/) {
    return env->NewIntArray(0);
}

// ---- Listing / opening assets ----

extern "C" JNIEXPORT jobjectArray JNICALL
Java_android_content_res_AssetManager_nativeList(
        JNIEnv* env, jclass, jlong /*amHandle*/, jstring /*path*/) {
    jclass strCls = env->FindClass("java/lang/String");
    if (strCls == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jobjectArray arr = env->NewObjectArray(0, strCls, nullptr);
    env->DeleteLocalRef(strCls);
    return arr;
}

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeOpenAsset(
        JNIEnv*, jclass, jlong /*amHandle*/, jstring /*fileName*/, jint /*mode*/) {
    return 0;
}

extern "C" JNIEXPORT jobject JNICALL
Java_android_content_res_AssetManager_nativeOpenAssetFd(
        JNIEnv*, jclass, jlong /*amHandle*/, jstring /*fileName*/,
        jlongArray /*outOffsets*/) {
    return nullptr;
}

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeOpenNonAsset(
        JNIEnv*, jclass, jlong /*amHandle*/, jint /*cookie*/,
        jstring /*fileName*/, jint /*mode*/) {
    return 0;
}

extern "C" JNIEXPORT jobject JNICALL
Java_android_content_res_AssetManager_nativeOpenNonAssetFd(
        JNIEnv*, jclass, jlong /*amHandle*/, jint /*cookie*/,
        jstring /*fileName*/, jlongArray /*outOffsets*/) {
    return nullptr;
}

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeOpenXmlAsset(
        JNIEnv*, jclass, jlong /*amHandle*/, jint /*cookie*/,
        jstring /*fileName*/) {
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_android_content_res_AssetManager_nativeOpenXmlAssetFd(
        JNIEnv*, jclass, jlong /*amHandle*/, jint /*cookie*/,
        jobject /*fileDescriptor*/) {
    return 0;
}

// ---- nativeSetApkAssets / nativeSetConfiguration / loggingEnabled ----

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeSetApkAssets(
        JNIEnv*, jclass, jlong /*amHandle*/, jobjectArray /*apkAssets*/,
        jboolean /*invalidateCaches*/, jboolean /*preserveOverlayHistory*/) {
}

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeSetConfiguration(
        JNIEnv*, jclass, jlong /*amHandle*/, jint /*mcc*/, jint /*mnc*/,
        jstring /*defaultLocale*/, jobjectArray /*locales*/,
        jint /*orientation*/, jint /*touchscreen*/, jint /*density*/,
        jint /*keyboard*/, jint /*keyboardHidden*/, jint /*navigation*/,
        jint /*screenWidth*/, jint /*screenHeight*/,
        jint /*smallestScreenWidthDp*/, jint /*screenWidthDp*/,
        jint /*screenHeightDp*/, jint /*screenLayout*/, jint /*uiMode*/,
        jint /*colorMode*/, jint /*grammaticalGender*/,
        jint /*majorVersion*/, jboolean /*forceRefresh*/) {
}

extern "C" JNIEXPORT void JNICALL
Java_android_content_res_AssetManager_nativeSetResourceResolutionLoggingEnabled(
        JNIEnv*, jclass, jlong /*amHandle*/, jboolean /*enabled*/) {
}

// ============================================================================
// JNI_OnLoad_assetmanager_with_cl — explicit RegisterNatives entry point.
//
// Called from binder_jni_stub.cc:JNI_OnLoad_binder_with_cl which is invoked
// from openjdk_stub.c:Runtime_nativeLoad when ServiceManager.<clinit>
// calls System.loadLibrary("android_runtime_stub"). The classLoader is the
// caller's classloader so we can look up framework.jar's AssetManager class
// even though it's on the bootclasspath.
// ============================================================================

// Helper: copy of binder_jni_stub's loadClassByName.
static jclass loadAMClassByName(JNIEnv* env, jobject classLoader, const char* dotted_name) {
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
JNI_OnLoad_assetmanager_with_cl(JavaVM* vm, jobject classLoader) {
    LOGI("JNI_OnLoad_assetmanager: vm=%p classLoader=%p", vm, classLoader);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        LOGE("JNI_OnLoad_assetmanager: GetEnv failed");
        return JNI_VERSION_1_6;
    }

    jclass cls = loadAMClassByName(env, classLoader, "android/content/res/AssetManager");
    if (cls == nullptr) {
        env->ExceptionClear();
        cls = env->FindClass("android/content/res/AssetManager");
    }
    if (cls == nullptr) {
        env->ExceptionClear();
        LOGE("JNI_OnLoad_assetmanager: FindClass(android/content/res/AssetManager) failed (CL=%p) — "
             "framework.jar not loaded yet; will be retried on next load",
             classLoader);
        return JNI_VERSION_1_6;
    }

    JNINativeMethod methods[] = {
        // Public static debug methods.
        {const_cast<char*>("getAssetAllocations"),
         const_cast<char*>("()Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_getAssetAllocations)},
        {const_cast<char*>("getGlobalAssetCount"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_getGlobalAssetCount)},
        {const_cast<char*>("getGlobalAssetManagerCount"),
         const_cast<char*>("()I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_getGlobalAssetManagerCount)},
        // nativeCreate / nativeDestroy.
        {const_cast<char*>("nativeCreate"),
         const_cast<char*>("()J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeCreate)},
        {const_cast<char*>("nativeDestroy"),
         const_cast<char*>("(J)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeDestroy)},
        // Theme management.
        {const_cast<char*>("nativeThemeCreate"),
         const_cast<char*>("(J)J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeThemeCreate)},
        {const_cast<char*>("nativeGetThemeFreeFunction"),
         const_cast<char*>("()J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetThemeFreeFunction)},
        {const_cast<char*>("nativeThemeApplyStyle"),
         const_cast<char*>("(JJIZ)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeThemeApplyStyle)},
        {const_cast<char*>("nativeThemeCopy"),
         const_cast<char*>("(JJJJ)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeThemeCopy)},
        {const_cast<char*>("nativeThemeDump"),
         const_cast<char*>("(JJILjava/lang/String;Ljava/lang/String;)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeThemeDump)},
        {const_cast<char*>("nativeThemeGetAttributeValue"),
         const_cast<char*>("(JJILandroid/util/TypedValue;Z)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeThemeGetAttributeValue)},
        {const_cast<char*>("nativeThemeGetChangingConfigurations"),
         const_cast<char*>("(J)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeThemeGetChangingConfigurations)},
        {const_cast<char*>("nativeThemeRebase"),
         const_cast<char*>("(JJ[I[ZI)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeThemeRebase)},
        // Asset I/O.
        {const_cast<char*>("nativeAssetDestroy"),
         const_cast<char*>("(J)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeAssetDestroy)},
        {const_cast<char*>("nativeAssetGetLength"),
         const_cast<char*>("(J)J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeAssetGetLength)},
        {const_cast<char*>("nativeAssetGetRemainingLength"),
         const_cast<char*>("(J)J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeAssetGetRemainingLength)},
        {const_cast<char*>("nativeAssetRead"),
         const_cast<char*>("(J[BII)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeAssetRead)},
        {const_cast<char*>("nativeAssetReadChar"),
         const_cast<char*>("(J)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeAssetReadChar)},
        {const_cast<char*>("nativeAssetSeek"),
         const_cast<char*>("(JJI)J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeAssetSeek)},
        // Attribute / style application.
        {const_cast<char*>("nativeApplyStyle"),
         const_cast<char*>("(JJIIJ[IJJ)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeApplyStyle)},
        {const_cast<char*>("nativeApplyStyleWithArray"),
         const_cast<char*>("(JJIIJ[I[I[I)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeApplyStyleWithArray)},
        {const_cast<char*>("nativeAttributeResolutionStack"),
         const_cast<char*>("(JJIII)[I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeAttributeResolutionStack)},
        {const_cast<char*>("nativeResolveAttrs"),
         const_cast<char*>("(JJII[I[I[I[I)Z"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeResolveAttrs)},
        {const_cast<char*>("nativeRetrieveAttributes"),
         const_cast<char*>("(JJ[I[I[I)Z"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeRetrieveAttributes)},
        // Resource lookup.
        {const_cast<char*>("nativeContainsAllocatedTable"),
         const_cast<char*>("(J)Z"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeContainsAllocatedTable)},
        {const_cast<char*>("nativeGetAssignedPackageIdentifiers"),
         const_cast<char*>("(JZZ)Landroid/util/SparseArray;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetAssignedPackageIdentifiers)},
        {const_cast<char*>("nativeGetLastResourceResolution"),
         const_cast<char*>("(J)Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetLastResourceResolution)},
        {const_cast<char*>("nativeGetLocales"),
         const_cast<char*>("(JZ)[Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetLocales)},
        {const_cast<char*>("nativeGetOverlayableMap"),
         const_cast<char*>("(JLjava/lang/String;)Ljava/util/Map;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetOverlayableMap)},
        {const_cast<char*>("nativeGetOverlayablesToString"),
         const_cast<char*>("(JLjava/lang/String;)Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetOverlayablesToString)},
        {const_cast<char*>("nativeGetParentThemeIdentifier"),
         const_cast<char*>("(JI)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetParentThemeIdentifier)},
        {const_cast<char*>("nativeGetResourceArray"),
         const_cast<char*>("(JI[I)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceArray)},
        {const_cast<char*>("nativeGetResourceArraySize"),
         const_cast<char*>("(JI)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceArraySize)},
        {const_cast<char*>("nativeGetResourceBagValue"),
         const_cast<char*>("(JIILandroid/util/TypedValue;)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceBagValue)},
        {const_cast<char*>("nativeGetResourceEntryName"),
         const_cast<char*>("(JI)Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceEntryName)},
        {const_cast<char*>("nativeGetResourceIdentifier"),
         const_cast<char*>("(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceIdentifier)},
        {const_cast<char*>("nativeGetResourceIntArray"),
         const_cast<char*>("(JI)[I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceIntArray)},
        {const_cast<char*>("nativeGetResourceName"),
         const_cast<char*>("(JI)Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceName)},
        {const_cast<char*>("nativeGetResourcePackageName"),
         const_cast<char*>("(JI)Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourcePackageName)},
        {const_cast<char*>("nativeGetResourceStringArray"),
         const_cast<char*>("(JI)[Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceStringArray)},
        {const_cast<char*>("nativeGetResourceStringArrayInfo"),
         const_cast<char*>("(JI)[I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceStringArrayInfo)},
        {const_cast<char*>("nativeGetResourceTypeName"),
         const_cast<char*>("(JI)Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceTypeName)},
        {const_cast<char*>("nativeGetResourceValue"),
         const_cast<char*>("(JISLandroid/util/TypedValue;Z)I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetResourceValue)},
        {const_cast<char*>("nativeGetSizeAndUiModeConfigurations"),
         const_cast<char*>("(J)[Landroid/content/res/Configuration;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetSizeAndUiModeConfigurations)},
        {const_cast<char*>("nativeGetSizeConfigurations"),
         const_cast<char*>("(J)[Landroid/content/res/Configuration;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetSizeConfigurations)},
        {const_cast<char*>("nativeGetStyleAttributes"),
         const_cast<char*>("(JI)[I"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeGetStyleAttributes)},
        // Asset listing / opening.
        {const_cast<char*>("nativeList"),
         const_cast<char*>("(JLjava/lang/String;)[Ljava/lang/String;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeList)},
        {const_cast<char*>("nativeOpenAsset"),
         const_cast<char*>("(JLjava/lang/String;I)J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeOpenAsset)},
        {const_cast<char*>("nativeOpenAssetFd"),
         const_cast<char*>("(JLjava/lang/String;[J)Landroid/os/ParcelFileDescriptor;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeOpenAssetFd)},
        {const_cast<char*>("nativeOpenNonAsset"),
         const_cast<char*>("(JILjava/lang/String;I)J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeOpenNonAsset)},
        {const_cast<char*>("nativeOpenNonAssetFd"),
         const_cast<char*>("(JILjava/lang/String;[J)Landroid/os/ParcelFileDescriptor;"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeOpenNonAssetFd)},
        {const_cast<char*>("nativeOpenXmlAsset"),
         const_cast<char*>("(JILjava/lang/String;)J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeOpenXmlAsset)},
        {const_cast<char*>("nativeOpenXmlAssetFd"),
         const_cast<char*>("(JILjava/io/FileDescriptor;)J"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeOpenXmlAssetFd)},
        // Set methods.
        {const_cast<char*>("nativeSetApkAssets"),
         const_cast<char*>("(J[Landroid/content/res/ApkAssets;ZZ)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeSetApkAssets)},
        {const_cast<char*>("nativeSetConfiguration"),
         const_cast<char*>("(JIILjava/lang/String;[Ljava/lang/String;IIIIIIIIIIIIIIIIZ)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeSetConfiguration)},
        {const_cast<char*>("nativeSetResourceResolutionLoggingEnabled"),
         const_cast<char*>("(JZ)V"),
         reinterpret_cast<void*>(Java_android_content_res_AssetManager_nativeSetResourceResolutionLoggingEnabled)},
    };
    int n = sizeof(methods) / sizeof(methods[0]);
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
            ++ok;
        } else {
            env->ExceptionClear();
            LOGE("JNI_OnLoad_assetmanager: RegisterNatives(android.content.res.AssetManager.%s%s) failed — "
                 "framework.jar AssetManager may not declare this native (older API level?)",
                 methods[i].name, methods[i].signature);
        }
    }
    LOGI("JNI_OnLoad_assetmanager: android.content.res.AssetManager natives: %d/%d", ok, n);
    env->DeleteLocalRef(cls);
    return JNI_VERSION_1_6;
}

// Legacy classloader-less entry.
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad_assetmanager(JavaVM* vm, void* /*reserved*/) {
    return JNI_OnLoad_assetmanager_with_cl(vm, nullptr);
}
