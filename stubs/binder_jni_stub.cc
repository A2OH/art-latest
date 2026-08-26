// SPDX-License-Identifier: Apache-2.0
//
// art-latest/stubs/binder_jni_stub.cc — Westlake M3-finish + M3++
//
// Statically linked JNI bridge between dalvikvm and libbinder.so.  Replaces
// the .so-loaded `libandroid_runtime_stub.cc` which fails on bionic-static
// dalvikvm because bionic's static `libdl.a` is a stub (dlopen returns NULL).
//
// Mirrors the existing OHBridge / MessageQueue / BinderInternal pattern:
// `Java_*` symbols below are auto-discovered by ART's JNI symbol resolver
// after `System.loadLibrary("android_runtime_stub")` is short-circuited to
// "success" in `Runtime_nativeLoad` (see openjdk_stub.c).
//
// Origin: aosp-libbinder-port/native/libandroid_runtime_stub.cc.  The two
// files share their JNI surface contracts; this file targets in-process
// static linking, the other (now unused for dalvikvm) targets .so loading.
//
// JNI surface implemented:
//   M3 (servicemanager glue):
//     android.os.ServiceManager.native{GetService, ListServices, AddService,
//                                      IsBinderAlive, ReleaseBinder,
//                                      BinderDescriptor}
//   M3++ (Java BBinder + same-process Stub.asInterface optimization):
//     android.os.Binder.{getNativeBBinderHolder, getNativeFinalizer,
//                        getCallingPid, getCallingUid}
//   HelloBinder.{println, eprintln}    (test harness only — bypasses
//                                       Java I/O which throws NPE in this
//                                       dalvikvm build)
//
// Implementation note: every entry calls `ensureInit()` to lazily set up
// the per-process ProcessState on /dev/vndbinder (overridable via the
// BINDER_DEVICE environment variable — matches sm_smoke / sandbox-boot.sh).

#include <jni.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mutex>
#include <string>

#include <binder/Binder.h>
#include <binder/IBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>

#include <utils/String8.h>
#include <utils/String16.h>
#include <utils/Vector.h>

#include <android/log.h>

#include "../../android-to-openharmony-migration/aosp-libbinder-port/native/JavaBBinderHolder.h"

using namespace android;

#define TAG "WLK-binder-jni"
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

// Heap-allocated wrapper so a Java jlong can carry a sp<IBinder> across the
// JNI boundary.  Released explicitly via nativeReleaseBinder so we don't
// rely on Java finalizers running.
struct BinderHandle {
    sp<IBinder> binder;
};

jlong toHandle(sp<IBinder> binder) {
    if (binder == nullptr) return 0;
    BinderHandle* h = new BinderHandle{std::move(binder)};
    return reinterpret_cast<jlong>(h);
}

BinderHandle* fromHandle(jlong h) {
    return reinterpret_cast<BinderHandle*>(h);
}

// CR1-fix: codex Tier 1 MEDIUM-1 -- gInitDone is touched from JNI entrypoints
// that can race when the threadpool started by initWithDriver dispatches an
// onTransact concurrent with main-thread Java ServiceManager calls.  Wrap
// the one-shot init in std::once_flag so ProcessState::initWithDriver and
// startThreadPool execute exactly once, and once is published-before any
// reader observes gInitDone=true.
static std::once_flag gInitOnce;
static bool gInitDone = false;

bool ensureInit() {
    std::call_once(gInitOnce, []() {
        const char* dev = getenv("BINDER_DEVICE");
        if (dev == nullptr || *dev == 0) dev = "/dev/vndbinder";
        LOGI("ensureInit: opening %s", dev);
        sp<ProcessState> ps = ProcessState::initWithDriver(dev);
        if (ps == nullptr) {
            LOGE("ensureInit: ProcessState::initWithDriver(%s) returned null", dev);
            return;
        }
        ps->startThreadPool();
        gInitDone = true;
        LOGI("ensureInit: ok");
    });
    return gInitDone;
}

String16 jstringToString16(JNIEnv* env, jstring js) {
    if (js == nullptr) return String16();
    const char* cs = env->GetStringUTFChars(js, nullptr);
    if (cs == nullptr) return String16();
    String16 result(cs);
    env->ReleaseStringUTFChars(js, cs);
    return result;
}

// Cached jclass + jfieldID for android.os.Binder.mObject.  Populated on
// first use of any Binder JNI call.  Without this, nativeAddService can't
// extract the JavaBBinderHolder pointer from a Java Binder.
struct BinderJavaOffsets {
    jclass cls;
    jfieldID mObjectField;
    jmethodID isInstanceMethod;
};
BinderJavaOffsets gBinderOffs = {nullptr, nullptr, nullptr};

// Lazy initialization for the Binder class lookup.  Pass the classloader if
// we have one (set by JNI_OnLoad_binder_with_cl); otherwise use FindClass
// which goes through the bootclasspath.
//
// CR1-fix: codex Tier 1 MEDIUM-2 -- guard the global ref allocation so a
// late call (after JNI_OnLoad_binder_with_cl already cached the class) does
// not leak a prior NewGlobalRef.  Identical pattern to JNI_OnLoad_binder_with_cl
// below: IsSameObject check, DeleteGlobalRef before re-alloc.
bool ensureBinderOffsets(JNIEnv* env, jobject classLoader) {
    if (gBinderOffs.cls != nullptr) return true;
    jclass cls = nullptr;
    if (classLoader != nullptr) {
        jclass clCls = env->GetObjectClass(classLoader);
        if (clCls != nullptr) {
            jmethodID mid = env->GetMethodID(clCls, "loadClass",
                                              "(Ljava/lang/String;)Ljava/lang/Class;");
            if (mid != nullptr) {
                jstring js = env->NewStringUTF("android.os.Binder");
                jobject c = env->CallObjectMethod(classLoader, mid, js);
                if (!env->ExceptionCheck()) {
                    cls = reinterpret_cast<jclass>(c);
                }
                env->ExceptionClear();
                env->DeleteLocalRef(js);
            }
            env->DeleteLocalRef(clCls);
        }
    }
    if (cls == nullptr) {
        cls = env->FindClass("android/os/Binder");
        if (cls == nullptr) { env->ExceptionClear(); return false; }
    }
    jfieldID f = env->GetFieldID(cls, "mObject", "J");
    if (f == nullptr) {
        env->ExceptionClear();
        LOGE("ensureBinderOffsets: android.os.Binder has no `long mObject` field");
        env->DeleteLocalRef(cls);
        return false;
    }
    // CR1-fix MEDIUM-2: another caller may have raced ahead and populated
    // gBinderOffs.cls while we were doing the FindClass dance.  If so, drop
    // our local ref and reuse the cached one (no leak).
    if (gBinderOffs.cls != nullptr) {
        if (!env->IsSameObject(gBinderOffs.cls, cls)) {
            // Race: someone else cached a different Binder.class via a
            // different classloader.  Keep the existing one; don't leak.
            LOGI("ensureBinderOffsets: race -- another thread populated cls; "
                 "discarding local copy");
        }
        env->DeleteLocalRef(cls);
        return true;
    }
    gBinderOffs.cls = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
    gBinderOffs.mObjectField = f;
    env->DeleteLocalRef(cls);
    LOGI("ensureBinderOffsets: cached Binder.mObject field=%p", f);
    return true;
}

// Helper: returns true if `obj` is an instance of android.os.Binder.
bool isInstanceOfBinder(JNIEnv* env, jobject obj) {
    if (obj == nullptr) return false;
    if (gBinderOffs.cls == nullptr) {
        if (!ensureBinderOffsets(env, nullptr)) return false;
    }
    return env->IsInstanceOf(obj, gBinderOffs.cls) == JNI_TRUE;
}

}  // namespace

// ===========================================================================
// android.os.ServiceManager.native* — auto-discovered by JNI symbol resolver
// ===========================================================================

extern "C" JNIEXPORT jlong JNICALL
Java_android_os_ServiceManager_nativeGetService(JNIEnv* env, jclass /*klass*/,
                                                 jstring jName) {
    if (!ensureInit()) return 0;
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == nullptr) {
        LOGE("nativeGetService: defaultServiceManager() returned null");
        return 0;
    }
    String16 name = jstringToString16(env, jName);
    sp<IBinder> b = sm->checkService(name);
    LOGI("nativeGetService(\"%s\") -> %p",
         String8(name).c_str(), b.get());
    return toHandle(b);
}

// M3++: Look up a service and, if it's a same-process JavaBBinder, return
// the originally-registered Java Binder object directly.  If it's a remote
// binder or a non-Java local binder, returns null — the caller should fall
// back to wrapping in a NativeBinderProxy via the legacy handle path.
extern "C" JNIEXPORT jobject JNICALL
Java_android_os_ServiceManager_nativeGetLocalService(JNIEnv* env, jclass /*klass*/,
                                                      jstring jName) {
    if (!ensureInit()) return nullptr;
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == nullptr) return nullptr;
    String16 name = jstringToString16(env, jName);
    sp<IBinder> b = sm->checkService(name);
    if (b == nullptr) return nullptr;
    jobject localJavaBinder = javaObjectForLocalIBinder(env, b);
    LOGI("nativeGetLocalService(\"%s\") binder=%p local=%p",
         String8(name).c_str(), b.get(), localJavaBinder);
    return localJavaBinder;
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_android_os_ServiceManager_nativeListServices(JNIEnv* env, jclass /*klass*/) {
    if (!ensureInit()) return nullptr;
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == nullptr) {
        LOGE("nativeListServices: defaultServiceManager() returned null");
        return nullptr;
    }
    Vector<String16> names = sm->listServices(IServiceManager::DUMP_FLAG_PRIORITY_ALL);
    LOGI("nativeListServices: %zu names", names.size());
    jclass stringCls = env->FindClass("java/lang/String");
    if (stringCls == nullptr) return nullptr;
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(names.size()),
                                            stringCls, nullptr);
    if (arr == nullptr) return nullptr;
    for (size_t i = 0; i < names.size(); ++i) {
        String8 n8(names[i]);
        jstring js = env->NewStringUTF(n8.c_str());
        env->SetObjectArrayElement(arr, static_cast<jsize>(i), js);
        env->DeleteLocalRef(js);
    }
    return arr;
}

// M3++: Now takes a Java IBinder.  If the IBinder is an android.os.Binder
// subclass (local), we extract its JavaBBinderHolder via the cached
// mObject field, promote to a sp<JavaBBinder>, and register THAT with
// servicemanager.
//
// CR1-fix: codex Tier 1 HIGH-2 -- previously, a non-null Java IBinder that
// wasn't an android.os.Binder subclass silently fell through to the anonymous
// BBinder fallback, masking broken registrations: later getService() would
// succeed but queryLocalInterface/transact wouldn't work on the recovered
// proxy.  Now: the anonymous fallback is reserved for the legitimate test
// path (javaBinder == nullptr, e.g. HelloBinder/sm_smoke legacy flow).  A
// non-null non-local IBinder returns BAD_TYPE so callers get a loud failure
// they can react to (ServiceManager.addService converts to RuntimeException).
extern "C" JNIEXPORT jint JNICALL
Java_android_os_ServiceManager_nativeAddService(JNIEnv* env, jclass /*klass*/,
                                                jstring jName, jobject javaBinder) {
    if (!ensureInit()) return -1;
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == nullptr) return -1;
    String16 name = jstringToString16(env, jName);
    sp<IBinder> toRegister;

    if (javaBinder != nullptr) {
        if (!isInstanceOfBinder(env, javaBinder)) {
            // CR1-fix #2: explicit reject -- a non-null Java object that is
            // NOT an android.os.Binder subclass cannot be promoted to a
            // JavaBBinder.  Registering an anonymous BBinder in its place
            // would silently mask the bug and break asInterface later.
            LOGE("nativeAddService(\"%s\"): javaBinder is not an "
                 "android.os.Binder subclass -- returning BAD_TYPE",
                 String8(name).c_str());
            // BAD_TYPE = -EINVAL in AOSP utils/Errors.h (= -22 on Linux).
            return static_cast<jint>(-EINVAL);
        }
        // Java android.os.Binder subclass — extract its JavaBBinderHolder.
        sp<IBinder> b = ibinderForJavaBinder(env, javaBinder,
                                              gBinderOffs.mObjectField);
        if (b == nullptr) {
            // Binder subclass with mObject==0 -- the Java side didn't run
            // Binder.<init>(), or the holder was already destroyed.  Loud
            // failure rather than silent recovery.
            LOGE("nativeAddService(\"%s\"): android.os.Binder has null "
                 "mObject -- holder unallocated, returning BAD_TYPE",
                 String8(name).c_str());
            return static_cast<jint>(-EINVAL);
        }
        toRegister = b;
        LOGI("nativeAddService(\"%s\"): registering local JavaBBinder=%p",
             String8(name).c_str(), b.get());
    } else {
        // M3 legacy path: HelloBinder / sm_smoke call addService(name, null)
        // to plant a name (no real service).  Anonymous BBinder token keeps
        // listServices honest but means transact/queryLocalInterface won't
        // work -- which is expected for these test-only entries.
        toRegister = sp<BBinder>::make();
        LOGI("nativeAddService(\"%s\"): null Java binder -- anonymous "
             "BBinder=%p (test-only path)",
             String8(name).c_str(), toRegister.get());
    }

    status_t st = sm->addService(name, toRegister, false /*allowIsolated*/,
                                 IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT);
    LOGI("nativeAddService(\"%s\") -> %d", String8(name).c_str(), st);
    return static_cast<jint>(st);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_android_os_ServiceManager_nativeIsBinderAlive(JNIEnv* /*env*/, jclass /*klass*/,
                                                    jlong handle) {
    BinderHandle* h = fromHandle(handle);
    if (h == nullptr) return JNI_FALSE;
    return h->binder->isBinderAlive() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_android_os_ServiceManager_nativeReleaseBinder(JNIEnv* /*env*/, jclass /*klass*/,
                                                    jlong handle) {
    BinderHandle* h = fromHandle(handle);
    if (h != nullptr) delete h;
}

extern "C" JNIEXPORT jstring JNICALL
Java_android_os_ServiceManager_nativeBinderDescriptor(JNIEnv* env, jclass /*klass*/,
                                                       jlong handle) {
    BinderHandle* h = fromHandle(handle);
    if (h == nullptr) return nullptr;
    String8 desc(h->binder->getInterfaceDescriptor());
    return env->NewStringUTF(desc.c_str());
}

// ===========================================================================
// android.os.Binder — M3++ native surface
// ===========================================================================
//
// Binder.<init> calls `getNativeBBinderHolder()` which mints a
// JavaBBinderHolder and returns its pointer (Java stores it in mObject).
// `getNativeFinalizer()` returns a function pointer the Java side calls when
// the Binder is GC'd to free the holder.  `getCallingUid`/`getCallingPid`
// return the JVM process's identity (since we don't yet have a transact path
// that establishes a calling identity — all calls are intra-process).

extern "C" JNIEXPORT jlong JNICALL
Java_android_os_Binder_getNativeBBinderHolder(JNIEnv* /*env*/, jobject /*self*/) {
    return westlake_create_javabinder_holder();
}

// Java passes this jlong to a Cleaner / finalizer to free the holder.
// We expose a tiny wrapper function whose address Java stores.  In a real
// AOSP build this is consumed by NativeAllocationRegistry; in M3++ the shim
// Binder.java calls it directly from finalize() (see shim/.../Binder.java).
//
// For simplicity we DON'T expose a function pointer (which would require
// platform-specific calling-convention plumbing on a `Cleaner`).  Instead,
// the shim Binder.java holds the holder pointer and calls
// `Java_android_os_Binder_nativeDestroy(long)` from finalize().
extern "C" JNIEXPORT jlong JNICALL
Java_android_os_Binder_getNativeFinalizer(JNIEnv* /*env*/, jclass /*klass*/) {
    // Return non-zero so callers that assert "finalizer is non-null" succeed.
    // Actual cleanup goes through Java_android_os_Binder_nativeDestroy below.
    return reinterpret_cast<jlong>(&westlake_destroy_javabinder_holder);
}

extern "C" JNIEXPORT void JNICALL
Java_android_os_Binder_nativeDestroy(JNIEnv* /*env*/, jclass /*klass*/,
                                      jlong handle) {
    westlake_destroy_javabinder_holder(handle);
}

extern "C" JNIEXPORT jint JNICALL
Java_android_os_Binder_getCallingPid(JNIEnv* /*env*/, jclass /*klass*/) {
    // For same-process binder calls there is no calling-pid distinction;
    // we report this process's PID.  When M4 wires up cross-process
    // onTransact, this should consult IPCThreadState::self()->getCallingPid().
    int pid = IPCThreadState::self()->getCallingPid();
    if (pid <= 0) pid = static_cast<int>(getpid());
    return pid;
}

extern "C" JNIEXPORT jint JNICALL
Java_android_os_Binder_getCallingUid(JNIEnv* /*env*/, jclass /*klass*/) {
    int uid = IPCThreadState::self()->getCallingUid();
    if (uid < 0) uid = static_cast<int>(geteuid());
    return uid;
}

extern "C" JNIEXPORT jlong JNICALL
Java_android_os_Binder_clearCallingIdentity(JNIEnv* /*env*/, jclass /*klass*/) {
    return IPCThreadState::self()->clearCallingIdentity();
}

extern "C" JNIEXPORT void JNICALL
Java_android_os_Binder_restoreCallingIdentity(JNIEnv* /*env*/, jclass /*klass*/,
                                                jlong token) {
    IPCThreadState::self()->restoreCallingIdentity(token);
}

extern "C" JNIEXPORT void JNICALL
Java_android_os_Binder_flushPendingCommands(JNIEnv* /*env*/, jclass /*klass*/) {
    IPCThreadState::self()->flushCommands();
}

// ===========================================================================
// HelloBinder.{println, eprintln} — System.out.println throws NPE in this
// dalvikvm (Charset.newEncoder returns null).  These trivial native helpers
// bypass the Java I/O stack so the M3 acceptance test can emit progress.
// ===========================================================================

extern "C" JNIEXPORT void JNICALL
Java_HelloBinder_println(JNIEnv* env, jclass /*klass*/, jstring js) {
    if (js == nullptr) {
        fprintf(stdout, "(null)\n");
        fflush(stdout);
        return;
    }
    const char* cs = env->GetStringUTFChars(js, nullptr);
    if (cs == nullptr) return;
    fprintf(stdout, "%s\n", cs);
    fflush(stdout);
    env->ReleaseStringUTFChars(js, cs);
}

extern "C" JNIEXPORT void JNICALL
Java_HelloBinder_eprintln(JNIEnv* env, jclass /*klass*/, jstring js) {
    if (js == nullptr) {
        fprintf(stderr, "(null)\n");
        return;
    }
    const char* cs = env->GetStringUTFChars(js, nullptr);
    if (cs == nullptr) return;
    fprintf(stderr, "%s\n", cs);
    env->ReleaseStringUTFChars(js, cs);
}

// AsInterfaceTest helpers — same shape as HelloBinder's, separate to keep
// the JNI registration table simple.
extern "C" JNIEXPORT void JNICALL
Java_AsInterfaceTest_println(JNIEnv* env, jclass /*klass*/, jstring js) {
    if (js == nullptr) { fprintf(stdout, "(null)\n"); fflush(stdout); return; }
    const char* cs = env->GetStringUTFChars(js, nullptr);
    if (cs == nullptr) return;
    fprintf(stdout, "%s\n", cs);
    fflush(stdout);
    env->ReleaseStringUTFChars(js, cs);
}

extern "C" JNIEXPORT void JNICALL
Java_AsInterfaceTest_eprintln(JNIEnv* env, jclass /*klass*/, jstring js) {
    if (js == nullptr) { fprintf(stderr, "(null)\n"); return; }
    const char* cs = env->GetStringUTFChars(js, nullptr);
    if (cs == nullptr) return;
    fprintf(stderr, "%s\n", cs);
    env->ReleaseStringUTFChars(js, cs);
}

// ActivityServiceTest helpers — M4a synthetic smoke test for
// WestlakeActivityManagerService.  Same shape as AsInterfaceTest's.
extern "C" JNIEXPORT void JNICALL
Java_ActivityServiceTest_println(JNIEnv* env, jclass /*klass*/, jstring js) {
    if (js == nullptr) { fprintf(stdout, "(null)\n"); fflush(stdout); return; }
    const char* cs = env->GetStringUTFChars(js, nullptr);
    if (cs == nullptr) return;
    fprintf(stdout, "%s\n", cs);
    fflush(stdout);
    env->ReleaseStringUTFChars(js, cs);
}

extern "C" JNIEXPORT void JNICALL
Java_ActivityServiceTest_eprintln(JNIEnv* env, jclass /*klass*/, jstring js) {
    if (js == nullptr) { fprintf(stderr, "(null)\n"); return; }
    const char* cs = env->GetStringUTFChars(js, nullptr);
    if (cs == nullptr) return;
    fprintf(stderr, "%s\n", cs);
    env->ReleaseStringUTFChars(js, cs);
}

// W2-discover helpers — NoiceDiscoverWrapper.{println,eprintln}.
// Used by the M4-discovery test wrapper that drives noice's bootstrap to
// surface which Binder services need to be implemented.  Same shape as
// HelloBinder / AsInterfaceTest helpers — no Java I/O encoder available.
extern "C" JNIEXPORT void JNICALL
Java_NoiceDiscoverWrapper_println(JNIEnv* env, jclass /*klass*/, jstring js) {
    if (js == nullptr) { fprintf(stdout, "(null)\n"); fflush(stdout); return; }
    const char* cs = env->GetStringUTFChars(js, nullptr);
    if (cs == nullptr) return;
    fprintf(stdout, "%s\n", cs);
    fflush(stdout);
    env->ReleaseStringUTFChars(js, cs);
}

extern "C" JNIEXPORT void JNICALL
Java_NoiceDiscoverWrapper_eprintln(JNIEnv* env, jclass /*klass*/, jstring js) {
    if (js == nullptr) { fprintf(stderr, "(null)\n"); return; }
    const char* cs = env->GetStringUTFChars(js, nullptr);
    if (cs == nullptr) return;
    fprintf(stderr, "%s\n", cs);
    env->ReleaseStringUTFChars(js, cs);
}

// Helper: ClassLoader.loadClass(name) → jclass.  Falls back to FindClass
// if `classLoader` is null.  Required because JNI_OnLoad runs in a thread
// context whose default JNIEnv FindClass uses the bootclasspath ClassLoader,
// which doesn't include classes loaded via `-cp` (e.g. aosp-shim.dex's
// android.os.ServiceManager and HelloBinder.dex's HelloBinder).
static jclass loadClassByName(JNIEnv* env, jobject classLoader, const char* dotted_name) {
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
    // loadClass takes the dotted name (java.lang.String, not java/lang/String).
    // The caller passes a slash-form name (FindClass shape); convert.
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

// JNI_OnLoad_binder: explicitly registers the binder JNI methods.
//
// ART's static build resolves Java natives via dlsym(handle, "Java_*").
// The fake "android_runtime_stub success" handle our short-circuit returns
// isn't a valid dlopen result, so dlsym returns NULL — meaning auto-
// discovery via name mangling does NOT work for statically-linked symbols.
// We must call RegisterNatives ourselves, the same way ohbridge_stub and
// framework_native_stubs do.
//
// Called from Runtime_nativeLoad in openjdk_stub.c when System.loadLibrary
// ("android_runtime_stub") is invoked from ServiceManager.java's <clinit>
// or HelloBinder.java's static block.  The optional classLoader arg comes
// from Runtime_nativeLoad's third parameter — when provided, we use it to
// load classes that live in app dex (HelloBinder) or in aosp-shim.dex on
// the system classpath (android.os.ServiceManager).  Safe to call multiple
// times — the second call's RegisterNatives just no-ops.
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad_binder_with_cl(JavaVM* vm, jobject classLoader) {
    LOGI("JNI_OnLoad_binder: vm=%p classLoader=%p", vm, classLoader);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        LOGE("JNI_OnLoad_binder: GetEnv failed");
        return JNI_VERSION_1_6;
    }

    // android.os.ServiceManager natives (provided by shim/java/android/os/ServiceManager.java).
    {
        jclass cls = loadClassByName(env, classLoader, "android/os/ServiceManager");
        if (cls == nullptr) {
            env->ExceptionClear();
            LOGE("JNI_OnLoad_binder: FindClass(android/os/ServiceManager) failed (CL=%p)", classLoader);
        } else {
            JNINativeMethod methods[] = {
                {const_cast<char*>("nativeGetService"),
                 const_cast<char*>("(Ljava/lang/String;)J"),
                 reinterpret_cast<void*>(Java_android_os_ServiceManager_nativeGetService)},
                {const_cast<char*>("nativeGetLocalService"),
                 const_cast<char*>("(Ljava/lang/String;)Landroid/os/IBinder;"),
                 reinterpret_cast<void*>(Java_android_os_ServiceManager_nativeGetLocalService)},
                {const_cast<char*>("nativeListServices"),
                 const_cast<char*>("()[Ljava/lang/String;"),
                 reinterpret_cast<void*>(Java_android_os_ServiceManager_nativeListServices)},
                {const_cast<char*>("nativeAddService"),
                 const_cast<char*>("(Ljava/lang/String;Landroid/os/IBinder;)I"),
                 reinterpret_cast<void*>(Java_android_os_ServiceManager_nativeAddService)},
                {const_cast<char*>("nativeIsBinderAlive"),
                 const_cast<char*>("(J)Z"),
                 reinterpret_cast<void*>(Java_android_os_ServiceManager_nativeIsBinderAlive)},
                {const_cast<char*>("nativeReleaseBinder"),
                 const_cast<char*>("(J)V"),
                 reinterpret_cast<void*>(Java_android_os_ServiceManager_nativeReleaseBinder)},
                {const_cast<char*>("nativeBinderDescriptor"),
                 const_cast<char*>("(J)Ljava/lang/String;"),
                 reinterpret_cast<void*>(Java_android_os_ServiceManager_nativeBinderDescriptor)},
            };
            int n = sizeof(methods) / sizeof(methods[0]);
            int ok = 0;
            for (int i = 0; i < n; ++i) {
                if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
                    ++ok;
                } else {
                    env->ExceptionClear();
                    LOGE("JNI_OnLoad_binder: RegisterNatives(android.os.ServiceManager.%s) failed",
                         methods[i].name);
                }
            }
            LOGI("JNI_OnLoad_binder: android.os.ServiceManager natives: %d/%d", ok, n);
            env->DeleteLocalRef(cls);
        }
    }

    // android.os.Binder natives (M3++) — getNativeBBinderHolder + the
    // identity natives.  Some Java code paths look these up reflectively;
    // we register them all explicitly here.
    {
        jclass cls = loadClassByName(env, classLoader, "android/os/Binder");
        if (cls == nullptr) {
            env->ExceptionClear();
            LOGE("JNI_OnLoad_binder: FindClass(android/os/Binder) failed (CL=%p)", classLoader);
        } else {
            // Cache field IDs while we're here — saves a class lookup later.
            jfieldID f = env->GetFieldID(cls, "mObject", "J");
            if (f != nullptr) {
                // CR1-fix: codex Tier 1 MEDIUM-2 -- JNI_OnLoad_binder_with_cl
                // is called by every `System.loadLibrary("android_runtime_stub")`
                // chain.  Without releasing the previous global ref before
                // creating a new one, every test that re-loads (HelloBinder,
                // AsInterfaceTest, PowerServiceTest, etc.) leaks a global ref
                // for Binder.class.  Delete the prior ref before replacing,
                // and skip the new alloc if we already have one for this
                // classloader (identity check via IsSameObject).
                if (gBinderOffs.cls != nullptr) {
                    if (env->IsSameObject(gBinderOffs.cls, cls)) {
                        // Same class from the same classloader -- keep the
                        // existing global ref, just refresh the field ID
                        // (cheap; same offset within the class).
                        gBinderOffs.mObjectField = f;
                        LOGI("JNI_OnLoad_binder: Binder.class already cached, "
                             "refreshed mObjectField=%p", f);
                    } else {
                        // Different class object (probably a different
                        // classloader) -- release the old global, install
                        // the new one.
                        LOGI("JNI_OnLoad_binder: replacing cached Binder.class "
                             "(classloader changed); deleting old global ref");
                        env->DeleteGlobalRef(gBinderOffs.cls);
                        gBinderOffs.cls = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
                        gBinderOffs.mObjectField = f;
                        LOGI("JNI_OnLoad_binder: cached Binder.mObject field=%p", f);
                    }
                } else {
                    gBinderOffs.cls = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
                    gBinderOffs.mObjectField = f;
                    LOGI("JNI_OnLoad_binder: cached Binder.mObject field=%p", f);
                }
            } else {
                env->ExceptionClear();
                LOGE("JNI_OnLoad_binder: android.os.Binder.mObject (J) not found");
            }

            JNINativeMethod methods[] = {
                {const_cast<char*>("getNativeBBinderHolder"),
                 const_cast<char*>("()J"),
                 reinterpret_cast<void*>(Java_android_os_Binder_getNativeBBinderHolder)},
                {const_cast<char*>("getNativeFinalizer"),
                 const_cast<char*>("()J"),
                 reinterpret_cast<void*>(Java_android_os_Binder_getNativeFinalizer)},
                {const_cast<char*>("nativeDestroy"),
                 const_cast<char*>("(J)V"),
                 reinterpret_cast<void*>(Java_android_os_Binder_nativeDestroy)},
                {const_cast<char*>("getCallingPid"),
                 const_cast<char*>("()I"),
                 reinterpret_cast<void*>(Java_android_os_Binder_getCallingPid)},
                {const_cast<char*>("getCallingUid"),
                 const_cast<char*>("()I"),
                 reinterpret_cast<void*>(Java_android_os_Binder_getCallingUid)},
                {const_cast<char*>("clearCallingIdentity"),
                 const_cast<char*>("()J"),
                 reinterpret_cast<void*>(Java_android_os_Binder_clearCallingIdentity)},
                {const_cast<char*>("restoreCallingIdentity"),
                 const_cast<char*>("(J)V"),
                 reinterpret_cast<void*>(Java_android_os_Binder_restoreCallingIdentity)},
                {const_cast<char*>("flushPendingCommands"),
                 const_cast<char*>("()V"),
                 reinterpret_cast<void*>(Java_android_os_Binder_flushPendingCommands)},
            };
            int n = sizeof(methods) / sizeof(methods[0]);
            int ok = 0;
            for (int i = 0; i < n; ++i) {
                if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
                    ++ok;
                } else {
                    env->ExceptionClear();
                    LOGE("JNI_OnLoad_binder: RegisterNatives(android.os.Binder.%s) failed — "
                         "shim Binder.java may not declare this native", methods[i].name);
                }
            }
            LOGI("JNI_OnLoad_binder: android.os.Binder natives: %d/%d", ok, n);
            env->DeleteLocalRef(cls);
        }
    }

    // HelloBinder test harness natives.  HelloBinder is in the app dex; if it
    // hasn't been loaded yet, FindClass returns null and we silently skip —
    // the class will trigger its own static init later which calls back here.
    {
        jclass cls = loadClassByName(env, classLoader, "HelloBinder");
        if (cls == nullptr) {
            env->ExceptionClear();
            LOGI("JNI_OnLoad_binder: HelloBinder not yet loaded (will register later)");
        } else {
            JNINativeMethod methods[] = {
                {const_cast<char*>("println"),
                 const_cast<char*>("(Ljava/lang/String;)V"),
                 reinterpret_cast<void*>(Java_HelloBinder_println)},
                {const_cast<char*>("eprintln"),
                 const_cast<char*>("(Ljava/lang/String;)V"),
                 reinterpret_cast<void*>(Java_HelloBinder_eprintln)},
            };
            int n = sizeof(methods) / sizeof(methods[0]);
            int ok = 0;
            for (int i = 0; i < n; ++i) {
                if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
                    ++ok;
                } else {
                    env->ExceptionClear();
                    LOGE("JNI_OnLoad_binder: RegisterNatives(HelloBinder.%s) failed",
                         methods[i].name);
                }
            }
            LOGI("JNI_OnLoad_binder: HelloBinder natives: %d/%d", ok, n);
            env->DeleteLocalRef(cls);
        }
    }

    // AsInterfaceTest natives (M3++ acceptance test).
    {
        jclass cls = loadClassByName(env, classLoader, "AsInterfaceTest");
        if (cls == nullptr) {
            env->ExceptionClear();
            LOGI("JNI_OnLoad_binder: AsInterfaceTest not yet loaded (will register later)");
        } else {
            JNINativeMethod methods[] = {
                {const_cast<char*>("println"),
                 const_cast<char*>("(Ljava/lang/String;)V"),
                 reinterpret_cast<void*>(Java_AsInterfaceTest_println)},
                {const_cast<char*>("eprintln"),
                 const_cast<char*>("(Ljava/lang/String;)V"),
                 reinterpret_cast<void*>(Java_AsInterfaceTest_eprintln)},
            };
            int n = sizeof(methods) / sizeof(methods[0]);
            int ok = 0;
            for (int i = 0; i < n; ++i) {
                if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
                    ++ok;
                } else {
                    env->ExceptionClear();
                    LOGE("JNI_OnLoad_binder: RegisterNatives(AsInterfaceTest.%s) failed",
                         methods[i].name);
                }
            }
            LOGI("JNI_OnLoad_binder: AsInterfaceTest natives: %d/%d", ok, n);
            env->DeleteLocalRef(cls);
        }
    }

    // ActivityServiceTest natives (M4a acceptance test).
    {
        jclass cls = loadClassByName(env, classLoader, "ActivityServiceTest");
        if (cls == nullptr) {
            env->ExceptionClear();
            LOGI("JNI_OnLoad_binder: ActivityServiceTest not yet loaded (will register later)");
        } else {
            JNINativeMethod methods[] = {
                {const_cast<char*>("println"),
                 const_cast<char*>("(Ljava/lang/String;)V"),
                 reinterpret_cast<void*>(Java_ActivityServiceTest_println)},
                {const_cast<char*>("eprintln"),
                 const_cast<char*>("(Ljava/lang/String;)V"),
                 reinterpret_cast<void*>(Java_ActivityServiceTest_eprintln)},
            };
            int n = sizeof(methods) / sizeof(methods[0]);
            int ok = 0;
            for (int i = 0; i < n; ++i) {
                if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
                    ++ok;
                } else {
                    env->ExceptionClear();
                    LOGE("JNI_OnLoad_binder: RegisterNatives(ActivityServiceTest.%s) failed",
                         methods[i].name);
                }
            }
            LOGI("JNI_OnLoad_binder: ActivityServiceTest natives: %d/%d", ok, n);
            env->DeleteLocalRef(cls);
        }
    }

    // NoiceDiscoverWrapper natives (W2-discover).
    {
        jclass cls = loadClassByName(env, classLoader, "NoiceDiscoverWrapper");
        if (cls == nullptr) {
            env->ExceptionClear();
            LOGI("JNI_OnLoad_binder: NoiceDiscoverWrapper not yet loaded (will register later)");
        } else {
            JNINativeMethod methods[] = {
                {const_cast<char*>("println"),
                 const_cast<char*>("(Ljava/lang/String;)V"),
                 reinterpret_cast<void*>(Java_NoiceDiscoverWrapper_println)},
                {const_cast<char*>("eprintln"),
                 const_cast<char*>("(Ljava/lang/String;)V"),
                 reinterpret_cast<void*>(Java_NoiceDiscoverWrapper_eprintln)},
            };
            int n = sizeof(methods) / sizeof(methods[0]);
            int ok = 0;
            for (int i = 0; i < n; ++i) {
                if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
                    ++ok;
                } else {
                    env->ExceptionClear();
                    LOGE("JNI_OnLoad_binder: RegisterNatives(NoiceDiscoverWrapper.%s) failed",
                         methods[i].name);
                }
            }
            LOGI("JNI_OnLoad_binder: NoiceDiscoverWrapper natives: %d/%d", ok, n);
            env->DeleteLocalRef(cls);
        }
    }

    // M4-PRE4 (2026-05-12): chain into messagequeue_jni_stub so framework.jar's
    // android.os.MessageQueue natives are wired up the same moment the
    // android_runtime_stub System.loadLibrary fires. Without this, the first
    // Looper.prepareMainLooper() call hits UnsatisfiedLinkError on nativeInit.
    {
        extern jint JNI_OnLoad_messagequeue_with_cl(JavaVM* vm, jobject classLoader);
        JNI_OnLoad_messagequeue_with_cl(vm, classLoader);
    }

    // M4-PRE7 (2026-05-12): chain into assetmanager_jni_stub so framework.jar's
    // android.content.res.AssetManager natives are wired up the same moment the
    // android_runtime_stub System.loadLibrary fires. Without this, the first
    // ResourcesImpl.<clinit> hits UnsatisfiedLinkError on
    // nativeGetThemeFreeFunction (called from sThemeRegistry initializer),
    // which then NPEs every subsequent Resources construction.
    {
        extern jint JNI_OnLoad_assetmanager_with_cl(JavaVM* vm, jobject classLoader);
        JNI_OnLoad_assetmanager_with_cl(vm, classLoader);
    }

    // M5-PRE (2026-05-12): chain into audiosystem_jni_stub so framework.jar's
    // android.media.AudioSystem natives are wired up the same moment the
    // android_runtime_stub System.loadLibrary fires. Without this,
    // AudioSystem.<clinit> hits UnsatisfiedLinkError on
    // native_getMaxChannelCount; ART tolerates the clinit failure but any
    // noice code touching AudioSystem (notification sound, MediaSession init,
    // AudioFocus) then NPEs.  Real audio routing is M5's audio daemon job;
    // this just unblocks the clinit.
    {
        extern jint JNI_OnLoad_audiosystem_with_cl(JavaVM* vm, jobject classLoader);
        JNI_OnLoad_audiosystem_with_cl(vm, classLoader);
    }

    return JNI_VERSION_1_6;
}

// Standard JNI_OnLoad shape: classloader-less entry point (legacy callers).
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad_binder(JavaVM* vm, void* /*reserved*/) {
    return JNI_OnLoad_binder_with_cl(vm, nullptr);
}
