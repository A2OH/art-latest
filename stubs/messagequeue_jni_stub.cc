// SPDX-License-Identifier: Apache-2.0
//
// art-latest/stubs/messagequeue_jni_stub.cc — Westlake M4-PRE4
//
// Statically linked JNI bridge between dalvikvm and android::Looper.
// Implements the 6 native methods declared by framework.jar's
// android.os.MessageQueue (Android 16 SDK), discovered missing in the
// M4-PRE3 noice-discover run where Looper.prepareMainLooper() failed with
// UnsatisfiedLinkError on MessageQueue.nativeInit.
//
// Mirrors the M3-finish pattern in binder_jni_stub.cc:
//   - Java_* symbols below are registered via env->RegisterNatives() in
//     JNI_OnLoad_messagequeue_with_cl() rather than relying on dlsym
//     (dalvikvm-bionic-static has a stub libdl).
//   - The registration entry point is called from
//     JNI_OnLoad_binder_with_cl() (binder_jni_stub.cc) so the moment the
//     System.loadLibrary("android_runtime_stub") short-circuit fires (in
//     ServiceManager.<clinit>) MessageQueue is also wired up.
//
// The C++ android::Looper class (from libutils) is already linked into
// dalvikvm via the libbinder_full_static.a whole-archive. We just provide
// the thin JNI glue, modeled on AOSP's
// frameworks/base/core/jni/android_os_MessageQueue.cpp.
//
// Java surface (framework.jar's classes3.dex android.os.MessageQueue):
//   nativeInit ()J                          PRIVATE STATIC NATIVE
//   nativeDestroy (J)V                      PRIVATE STATIC NATIVE
//   nativePollOnce (JI)V                    PRIVATE NATIVE (instance)
//   nativeWake (J)V                         PRIVATE STATIC NATIVE
//   nativeIsPolling (J)Z                    PRIVATE STATIC NATIVE
//   nativeSetFileDescriptorEvents (JII)V    PRIVATE STATIC NATIVE
//
// Note nativePollOnce is an INSTANCE method (no static bit). The JNI
// signature matches: jobject is the MessageQueue instance, jlong is the
// ptr field, jint is the timeout.

#include <jni.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utils/Looper.h>
#include <utils/RefBase.h>

#include <android/log.h>

using namespace android;

#define TAG "WLK-mq-jni"
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

// Must be kept in sync with the constants in
// android.os.MessageQueue.OnFileDescriptorEventListener.EVENT_*.
static constexpr int CALLBACK_EVENT_INPUT  = 1 << 0;
static constexpr int CALLBACK_EVENT_OUTPUT = 1 << 1;
static constexpr int CALLBACK_EVENT_ERROR  = 1 << 2;

// Cached method id for MessageQueue.dispatchEvents(int fd, int events) -> int.
// Initialized lazily on first nativeSetFileDescriptorEvents call. If the
// dispatchEvents method is not present (older framework.jar) we silently
// no-op the FD-event delivery path.
static jmethodID gDispatchEventsMid = nullptr;

// NativeMessageQueue wraps an android::Looper and the JNI plumbing needed to
// deliver epoll-detected fd events back to Java via MessageQueue.dispatchEvents.
//
// Pattern lifted from AOSP's android_os_MessageQueue.cpp:NativeMessageQueue.
// We omit MessageQueue::raiseException (the AOSP base class) and inline the
// exception handling here.
class NativeMessageQueue : public LooperCallback {
public:
    NativeMessageQueue();

    sp<Looper> getLooper() const { return mLooper; }

    void pollOnce(JNIEnv* env, jobject obj, int timeoutMillis);
    void wake();
    void setFileDescriptorEvents(int fd, int events);

    // LooperCallback override.
    int handleEvent(int fd, int events, void* data) override;

protected:
    virtual ~NativeMessageQueue();

private:
    sp<Looper> mLooper;

    // State held only during pollOnce so handleEvent() can call back into
    // Java on the right MessageQueue instance.
    JNIEnv* mPollEnv;
    jobject mPollObj;
    jthrowable mExceptionObj;
};

NativeMessageQueue::NativeMessageQueue()
        : mPollEnv(nullptr), mPollObj(nullptr), mExceptionObj(nullptr) {
    // Reuse the thread's Looper if one already exists (matches AOSP semantics
    // where MessageQueue.<init>(quitAllowed) is called from Looper.<init>,
    // which is created on the calling thread).
    mLooper = Looper::getForThread();
    if (mLooper == nullptr) {
        // allowNonCallbacks=true: MessageQueue.addOnFileDescriptorEventListener
        // requires this — fd-only events are dispatched back without callbacks.
        mLooper = new Looper(true);
        Looper::setForThread(mLooper);
    }
}

NativeMessageQueue::~NativeMessageQueue() {}

void NativeMessageQueue::pollOnce(JNIEnv* env, jobject pollObj, int timeoutMillis) {
    mPollEnv = env;
    mPollObj = pollObj;
    mLooper->pollOnce(timeoutMillis);
    mPollObj = nullptr;
    mPollEnv = nullptr;

    if (mExceptionObj) {
        env->Throw(mExceptionObj);
        env->DeleteLocalRef(mExceptionObj);
        mExceptionObj = nullptr;
    }
}

void NativeMessageQueue::wake() {
    mLooper->wake();
}

void NativeMessageQueue::setFileDescriptorEvents(int fd, int events) {
    if (events) {
        int looperEvents = 0;
        if (events & CALLBACK_EVENT_INPUT)  looperEvents |= Looper::EVENT_INPUT;
        if (events & CALLBACK_EVENT_OUTPUT) looperEvents |= Looper::EVENT_OUTPUT;
        mLooper->addFd(fd, Looper::POLL_CALLBACK, looperEvents, this,
                       reinterpret_cast<void*>(static_cast<intptr_t>(events)));
    } else {
        mLooper->removeFd(fd);
    }
}

int NativeMessageQueue::handleEvent(int fd, int looperEvents, void* data) {
    int events = 0;
    if (looperEvents & Looper::EVENT_INPUT)  events |= CALLBACK_EVENT_INPUT;
    if (looperEvents & Looper::EVENT_OUTPUT) events |= CALLBACK_EVENT_OUTPUT;
    if (looperEvents & (Looper::EVENT_ERROR | Looper::EVENT_HANGUP |
                        Looper::EVENT_INVALID)) {
        events |= CALLBACK_EVENT_ERROR;
    }
    int oldWatchedEvents = static_cast<int>(reinterpret_cast<intptr_t>(data));

    if (mPollEnv == nullptr || mPollObj == nullptr || gDispatchEventsMid == nullptr) {
        // Stale callback (not inside a pollOnce window) or no Java dispatch
        // method to call. Unregister the fd to keep epoll clean.
        return 0;
    }

    int newWatchedEvents = mPollEnv->CallIntMethod(mPollObj, gDispatchEventsMid,
                                                   fd, events);
    if (mPollEnv->ExceptionCheck()) {
        // Capture exception so it's re-thrown after pollOnce returns. AOSP
        // does the same — exceptions from dispatchEvents propagate up.
        mExceptionObj = mPollEnv->ExceptionOccurred();
        mPollEnv->ExceptionClear();
        return 0;
    }
    if (!newWatchedEvents) return 0;  // unregister the fd
    if (newWatchedEvents != oldWatchedEvents) {
        setFileDescriptorEvents(fd, newWatchedEvents);
    }
    return 1;
}

}  // namespace

// ============================================================================
// JNI entrypoints — these are the Java_android_os_MessageQueue_native* names
// the framework MessageQueue.class declares. Registered via RegisterNatives
// in JNI_OnLoad_messagequeue_with_cl below.
// ============================================================================

extern "C" JNIEXPORT jlong JNICALL
Java_android_os_MessageQueue_nativeInit(JNIEnv* env, jclass /*clazz*/) {
    NativeMessageQueue* nq = new NativeMessageQueue();
    if (nq == nullptr) {
        // Throw RuntimeException, like AOSP's jniThrowRuntimeException does.
        jclass rex = env->FindClass("java/lang/RuntimeException");
        if (rex) env->ThrowNew(rex, "Unable to allocate native MessageQueue");
        return 0;
    }
    nq->incStrong(env);  // Java owns one strong ref; released in nativeDestroy.
    jlong ptr = reinterpret_cast<jlong>(nq);
    LOGI("nativeInit -> NativeMessageQueue=%p looper=%p tid=%ld",
         nq, nq->getLooper().get(), (long)pthread_self());
    return ptr;
}

extern "C" JNIEXPORT void JNICALL
Java_android_os_MessageQueue_nativeDestroy(JNIEnv* env, jclass /*clazz*/, jlong ptr) {
    NativeMessageQueue* nq = reinterpret_cast<NativeMessageQueue*>(ptr);
    if (nq == nullptr) return;
    nq->decStrong(env);  // RefBase drops to zero -> ~NativeMessageQueue
}

extern "C" JNIEXPORT void JNICALL
Java_android_os_MessageQueue_nativePollOnce(JNIEnv* env, jobject obj,
                                             jlong ptr, jint timeoutMillis) {
    NativeMessageQueue* nq = reinterpret_cast<NativeMessageQueue*>(ptr);
    if (nq == nullptr) return;
    nq->pollOnce(env, obj, timeoutMillis);
}

extern "C" JNIEXPORT void JNICALL
Java_android_os_MessageQueue_nativeWake(JNIEnv* /*env*/, jclass /*clazz*/, jlong ptr) {
    NativeMessageQueue* nq = reinterpret_cast<NativeMessageQueue*>(ptr);
    if (nq == nullptr) return;
    nq->wake();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_android_os_MessageQueue_nativeIsPolling(JNIEnv* /*env*/, jclass /*clazz*/, jlong ptr) {
    NativeMessageQueue* nq = reinterpret_cast<NativeMessageQueue*>(ptr);
    if (nq == nullptr) return JNI_FALSE;
    return nq->getLooper()->isPolling() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_android_os_MessageQueue_nativeSetFileDescriptorEvents(JNIEnv* /*env*/, jclass /*clazz*/,
                                                            jlong ptr, jint fd, jint events) {
    NativeMessageQueue* nq = reinterpret_cast<NativeMessageQueue*>(ptr);
    if (nq == nullptr) return;
    nq->setFileDescriptorEvents(fd, events);
}

// ============================================================================
// JNI_OnLoad_messagequeue_with_cl — explicit RegisterNatives entry point.
//
// Called from binder_jni_stub.cc:JNI_OnLoad_binder_with_cl which itself is
// called from openjdk_stub.c:Runtime_nativeLoad when ServiceManager.<clinit>
// calls System.loadLibrary("android_runtime_stub"). The classLoader is the
// caller's classloader so we can look up the framework.jar MessageQueue
// class even though it's on the bootclasspath (FindClass should also work
// but we accept either).
//
// Returns 0 if registered all 6 methods successfully; positive count
// otherwise. Errors are logged and tolerated — a missing class or method is
// not fatal (the discovery harness may run before framework.jar is fully
// resolved; we'll be called again on the next library load).
// ============================================================================

// Helper: copy of binder_jni_stub's loadClassByName so we don't depend on
// the binder TU's internal linkage. Falls back to FindClass.
static jclass loadMQClassByName(JNIEnv* env, jobject classLoader, const char* dotted_name) {
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
JNI_OnLoad_messagequeue_with_cl(JavaVM* vm, jobject classLoader) {
    LOGI("JNI_OnLoad_messagequeue: vm=%p classLoader=%p", vm, classLoader);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        LOGE("JNI_OnLoad_messagequeue: GetEnv failed");
        return JNI_VERSION_1_6;
    }

    jclass cls = loadMQClassByName(env, classLoader, "android/os/MessageQueue");
    if (cls == nullptr) {
        env->ExceptionClear();
        // Try FindClass as a last resort (system classloader).
        cls = env->FindClass("android/os/MessageQueue");
    }
    if (cls == nullptr) {
        env->ExceptionClear();
        LOGE("JNI_OnLoad_messagequeue: FindClass(android/os/MessageQueue) failed (CL=%p) — "
             "framework.jar not loaded yet; will be retried on next load",
             classLoader);
        return JNI_VERSION_1_6;
    }

    JNINativeMethod methods[] = {
        {const_cast<char*>("nativeInit"),
         const_cast<char*>("()J"),
         reinterpret_cast<void*>(Java_android_os_MessageQueue_nativeInit)},
        {const_cast<char*>("nativeDestroy"),
         const_cast<char*>("(J)V"),
         reinterpret_cast<void*>(Java_android_os_MessageQueue_nativeDestroy)},
        {const_cast<char*>("nativePollOnce"),
         const_cast<char*>("(JI)V"),
         reinterpret_cast<void*>(Java_android_os_MessageQueue_nativePollOnce)},
        {const_cast<char*>("nativeWake"),
         const_cast<char*>("(J)V"),
         reinterpret_cast<void*>(Java_android_os_MessageQueue_nativeWake)},
        {const_cast<char*>("nativeIsPolling"),
         const_cast<char*>("(J)Z"),
         reinterpret_cast<void*>(Java_android_os_MessageQueue_nativeIsPolling)},
        {const_cast<char*>("nativeSetFileDescriptorEvents"),
         const_cast<char*>("(JII)V"),
         reinterpret_cast<void*>(Java_android_os_MessageQueue_nativeSetFileDescriptorEvents)},
    };
    int n = sizeof(methods) / sizeof(methods[0]);
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
            ++ok;
        } else {
            env->ExceptionClear();
            LOGE("JNI_OnLoad_messagequeue: RegisterNatives(android.os.MessageQueue.%s%s) failed — "
                 "framework.jar MessageQueue may not declare this native (older API level?)",
                 methods[i].name, methods[i].signature);
        }
    }

    // Cache MessageQueue.dispatchEvents method id for handleEvent.
    jmethodID dispatch = env->GetMethodID(cls, "dispatchEvents", "(II)I");
    if (dispatch != nullptr) {
        gDispatchEventsMid = dispatch;
        LOGI("JNI_OnLoad_messagequeue: cached MessageQueue.dispatchEvents mid=%p", dispatch);
    } else {
        env->ExceptionClear();
        LOGI("JNI_OnLoad_messagequeue: MessageQueue.dispatchEvents not present — "
             "fd-event delivery will be a no-op (still safe; just no callbacks)");
    }

    LOGI("JNI_OnLoad_messagequeue: android.os.MessageQueue natives: %d/%d", ok, n);
    env->DeleteLocalRef(cls);
    return JNI_VERSION_1_6;
}

// Legacy classloader-less entry (for any caller that doesn't know the CL).
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad_messagequeue(JavaVM* vm, void* /*reserved*/) {
    return JNI_OnLoad_messagequeue_with_cl(vm, nullptr);
}
