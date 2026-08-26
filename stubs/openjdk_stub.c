#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>

/* Register native methods one at a time, skipping failures */
static int registerNativesOrSkip(JNIEnv* env, jclass clazz,
                                  const JNINativeMethod* methods, int numMethods) {
    int registered = 0;
    for (int i = 0; i < numMethods; i++) {
        if ((*env)->RegisterNatives(env, clazz, &methods[i], 1) == 0) {
            registered++;
        } else {
            (*env)->ExceptionClear(env);
        }
    }
    return registered;
}

/* ==================== java.lang.System natives ==================== */

static jobjectArray System_specialProperties(JNIEnv* env, jclass ignored) {
    /* On the standalone guest path, constructing a String[] in native code can
     * cross class-loader boundaries badly enough to trip ArrayStoreException at
     * the JNI return boundary. Reuse the boot-owned EmptyArray.STRING object
     * instead of materializing a new array here. */
    jclass empty_array_class = (*env)->FindClass(env, "libcore/util/EmptyArray");
    if (!empty_array_class || (*env)->ExceptionCheck(env)) return NULL;
    jfieldID string_field =
        (*env)->GetStaticFieldID(env, empty_array_class, "STRING", "[Ljava/lang/String;");
    if (!string_field || (*env)->ExceptionCheck(env)) return NULL;
    return (jobjectArray)(*env)->GetStaticObjectField(env, empty_array_class, string_field);
}

static jlong System_nanoTime(JNIEnv* env, jclass ignored) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (jlong)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static jlong System_currentTimeMillis(JNIEnv* env, jclass ignored) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (jlong)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static jstring System_mapLibraryName(JNIEnv* env, jclass ignored, jstring libname) {
    if (!libname) return NULL;
    const char* name = (*env)->GetStringUTFChars(env, libname, NULL);
    char buf[512];
    snprintf(buf, sizeof(buf), "lib%s.so", name);
    (*env)->ReleaseStringUTFChars(env, libname, name);
    return (*env)->NewStringUTF(env, buf);
}

static void System_log(JNIEnv* env, jclass ignored, jchar type, jstring msg, jthrowable exc) {
    if (msg) {
        const char* s = (*env)->GetStringUTFChars(env, msg, NULL);
        fprintf(stderr, "System.log(%c): %s\n", (char)type, s);
        (*env)->ReleaseStringUTFChars(env, msg, s);
    }
}

static void System_setErr0(JNIEnv* env, jclass clazz, jobject stream) { }
static void System_setOut0(JNIEnv* env, jclass clazz, jobject stream) { }
static void System_setIn0(JNIEnv* env, jclass clazz, jobject stream) { }

/* ==================== sun.misc.Version natives ==================== */

static jstring Version_getJvmSpecialVersion(JNIEnv* env, jclass clazz) {
    return (*env)->NewStringUTF(env, "");
}
static jstring Version_getJdkSpecialVersion(JNIEnv* env, jclass clazz) {
    return (*env)->NewStringUTF(env, "");
}
static jboolean Version_getJvmVersionInfo(JNIEnv* env, jclass clazz) {
    return JNI_FALSE;
}
static void Version_getJdkVersionInfo(JNIEnv* env, jclass clazz) { }

/* ==================== java.io.FileDescriptor natives ==================== */

static jboolean FileDescriptor_getAppend(JNIEnv* env, jclass clazz, jint fd) {
    int flags = fcntl(fd, F_GETFL);
    return (flags != -1 && (flags & O_APPEND)) ? JNI_TRUE : JNI_FALSE;
}

static jboolean FileDescriptor_isSocket(JNIEnv* env, jclass clazz, jint fd) {
    struct stat sb;
    if (fstat(fd, &sb) == 0) {
        return S_ISSOCK(sb.st_mode) ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

static int getChannelFd(JNIEnv* env, jobject fdObj);
static void setChannelFd(JNIEnv* env, jobject fdObj, jint fd);

static void FileDescriptor_sync(JNIEnv* env, jobject thiz) {
    int fd = getChannelFd(env, thiz);
    if (fd >= 0) {
        if (fsync(fd) < 0) {
            jclass ioExCls = (*env)->FindClass(env, "java/io/SyncFailedException");
            if (ioExCls) {
                (*env)->ThrowNew(env, ioExCls, strerror(errno));
            }
        }
    }
}

/* ==================== sun.nio.ch.FileDispatcherImpl natives ==================== */

static int getChannelFd(JNIEnv* env, jobject fdObj) {
    if (!fdObj) return -1;
    jclass fdCls = (*env)->GetObjectClass(env, fdObj);
    jfieldID descField = (*env)->GetFieldID(env, fdCls, "descriptor", "I");
    if (!descField) {
        (*env)->ExceptionClear(env);
        descField = (*env)->GetFieldID(env, fdCls, "fd", "I");
    }
    if (!descField) return -1;
    return (*env)->GetIntField(env, fdObj, descField);
}

static void setChannelFd(JNIEnv* env, jobject fdObj, jint fd) {
    if (!fdObj) return;
    jclass fdCls = (*env)->GetObjectClass(env, fdObj);
    jfieldID descField = (*env)->GetFieldID(env, fdCls, "descriptor", "I");
    if (!descField) {
        (*env)->ExceptionClear(env);
        descField = (*env)->GetFieldID(env, fdCls, "fd", "I");
    }
    if (descField) {
        (*env)->SetIntField(env, fdObj, descField, fd);
    }
}

static void FileDispatcherImpl_init(JNIEnv* env, jclass clazz) {
    /* no-op */
}

static jint FileDispatcherImpl_read0(JNIEnv* env, jobject thiz, jobject fdObj, jlong address, jint len) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    ssize_t n = read(fd, (void*)(uintptr_t)address, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; /* IOS_UNAVAILABLE */
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return -1;
    }
    return (n == 0) ? -1 : (jint)n; /* -1 = EOF */
}

static jint FileDispatcherImpl_write0(JNIEnv* env, jobject thiz, jobject fdObj, jlong address, jint len) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    ssize_t n = write(fd, (void*)(uintptr_t)address, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return -1;
    }
    return (jint)n;
}

static jint FileDispatcherImpl_pread0(JNIEnv* env, jobject thiz, jobject fdObj, jlong address, jint len, jlong offset) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    ssize_t n = pread(fd, (void*)(uintptr_t)address, len, (off_t)offset);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return -1;
    }
    return (n == 0) ? -1 : (jint)n;
}

static jint FileDispatcherImpl_pwrite0(JNIEnv* env, jobject thiz, jobject fdObj, jlong address, jint len, jlong offset) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    ssize_t n = pwrite(fd, (void*)(uintptr_t)address, len, (off_t)offset);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return -1;
    }
    return (jint)n;
}

static jlong FileDispatcherImpl_size0(JNIEnv* env, jobject thiz, jobject fdObj) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    struct stat sb;
    if (fstat(fd, &sb) < 0) return -1;
    return (jlong)sb.st_size;
}

static void FileDispatcherImpl_close0(JNIEnv* env, jobject thiz, jobject fdObj) {
    int fd = getChannelFd(env, fdObj);
    if (fd >= 0) close(fd);
}

static jint FileDispatcherImpl_force0(JNIEnv* env, jobject thiz, jobject fdObj, jboolean metadata) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    int rc = metadata ? fsync(fd) : fdatasync(fd);
    if (rc < 0) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return -1;
    }
    return 0;
}

static jint FileDispatcherImpl_truncate0(JNIEnv* env, jobject thiz, jobject fdObj, jlong size) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return -1;
    }
    return 0;
}

static jlong FileDispatcherImpl_seek0(JNIEnv* env, jobject thiz, jobject fdObj, jlong offset) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    off_t result = lseek(fd, (off_t)offset, SEEK_SET);
    if (result == (off_t)-1) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return -1;
    }
    return (jlong)result;
}

static jint FileDispatcherImpl_lock0(JNIEnv* env,
                                     jobject thiz,
                                     jobject fdObj,
                                     jboolean blocking,
                                     jlong pos,
                                     jlong size,
                                     jboolean shared) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1; /* NO_LOCK */

    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = shared ? F_RDLCK : F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = (off_t)pos;
    lock.l_len = (off_t)size;
    int cmd = blocking ? F_SETLKW : F_SETLK;
    if (fcntl(fd, cmd, &lock) == 0) {
        return 0; /* LOCKED */
    }
    if (errno == EINTR) {
        return 2; /* INTERRUPTED */
    }
    if (errno == EACCES || errno == EAGAIN) {
        return -1; /* NO_LOCK */
    }
    jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
    if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
    return -1;
}

static void FileDispatcherImpl_release0(JNIEnv* env,
                                        jobject thiz,
                                        jobject fdObj,
                                        jlong pos,
                                        jlong size) {
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return;
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = (off_t)pos;
    lock.l_len = (off_t)size;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
    }
}

static void FileDispatcherImpl_closeIntFD(JNIEnv* env, jclass clazz, jint fd) {
    if (fd >= 0) close(fd);
}

static jlong FileDispatcherImpl_allocationGranularity0(JNIEnv* env, jclass clazz) {
    long page_size = sysconf(_SC_PAGESIZE);
    return (jlong)(page_size > 0 ? page_size : 4096);
}

static jlong FileDispatcherImpl_map0(JNIEnv* env,
                                     jobject thiz,
                                     jobject fdObj,
                                     jint prot,
                                     jlong position,
                                     jlong length,
                                     jboolean sync) {
    (void)sync;
    int fd = getChannelFd(env, fdObj);
    if (fd < 0 || length <= 0 || position < 0) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, "invalid map request");
        return (jlong)-1;
    }
    int mmap_prot = PROT_READ;
    int mmap_flags = MAP_SHARED;
    if (prot == 1) {
        mmap_prot = PROT_READ | PROT_WRITE;
        mmap_flags = MAP_SHARED;
    } else if (prot == 2) {
        mmap_prot = PROT_READ | PROT_WRITE;
        mmap_flags = MAP_PRIVATE;
    }
    void* addr = mmap(NULL, (size_t)length, mmap_prot, mmap_flags, fd, (off_t)position);
    if (addr == MAP_FAILED) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return (jlong)-1;
    }
    return (jlong)(uintptr_t)addr;
}

static jint FileDispatcherImpl_setDirect0(JNIEnv* env, jobject thiz, jobject fdObj) {
    (void)env;
    (void)thiz;
    (void)fdObj;
    return 0;
}

/* ==================== sun.nio.ch.FileChannelImpl natives ==================== */

static jlong FileChannelImpl_initIDs(JNIEnv* env, jclass clazz) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    fprintf(stderr, "[PFCUT] FileChannelImpl.initIDs -> %ld\n", page_size);
    return (jlong)page_size;
}

static jlong FileChannelImpl_map0(JNIEnv* env, jobject thiz,
                                  jint prot, jlong position, jlong length) {
    if (!thiz || length <= 0 || position < 0) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, "invalid map request");
        return (jlong)-1;
    }

    jclass cls = (*env)->GetObjectClass(env, thiz);
    jfieldID fdField = (*env)->GetFieldID(env, cls, "fd", "Ljava/io/FileDescriptor;");
    if (!fdField) return (jlong)-1;
    jobject fdObj = (*env)->GetObjectField(env, thiz, fdField);
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, "invalid file descriptor");
        return (jlong)-1;
    }

    int mmap_prot = PROT_READ;
    int mmap_flags = MAP_SHARED;
    if (prot == 1) {          /* FileChannelImpl.MAP_RW */
        mmap_prot = PROT_READ | PROT_WRITE;
        mmap_flags = MAP_SHARED;
    } else if (prot == 2) {   /* FileChannelImpl.MAP_PV */
        mmap_prot = PROT_READ | PROT_WRITE;
        mmap_flags = MAP_PRIVATE;
    }

    void* addr = mmap(NULL,
                      (size_t)length,
                      mmap_prot,
                      mmap_flags,
                      fd,
                      (off_t)position);
    if (addr == MAP_FAILED) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return (jlong)-1;
    }

    static int map_log_count = 0;
    if (map_log_count < 80) {
        map_log_count++;
        fprintf(stderr,
                "[PFCUT] FileChannelImpl.map0 fd=%d prot=%d pos=%lld len=%lld addr=%p\n",
                fd,
                prot,
                (long long)position,
                (long long)length,
                addr);
    }
    return (jlong)(uintptr_t)addr;
}

static jint FileChannelImpl_unmap0(JNIEnv* env, jclass clazz, jlong address, jlong length) {
    if (address == 0 || length <= 0) return 0;
    int rc = munmap((void*)(uintptr_t)address, (size_t)length);
    if (rc != 0) {
        jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioExCls) (*env)->ThrowNew(env, ioExCls, strerror(errno));
        return -1;
    }
    return 0;
}

/* ==================== sun.nio.ch.NativeThread natives ==================== */

static jlong NativeThread_current(JNIEnv* env, jclass clazz) {
    return (jlong)-1;
}

static void NativeThread_signal(JNIEnv* env, jclass clazz, jlong thread) {
    (void)thread;
}

/* ==================== sun.nio.fs.UnixNativeDispatcher natives ==================== */

static jfieldID und_attrs_st_mode;
static jfieldID und_attrs_st_ino;
static jfieldID und_attrs_st_dev;
static jfieldID und_attrs_st_rdev;
static jfieldID und_attrs_st_nlink;
static jfieldID und_attrs_st_uid;
static jfieldID und_attrs_st_gid;
static jfieldID und_attrs_st_size;
static jfieldID und_attrs_st_atime_sec;
static jfieldID und_attrs_st_atime_nsec;
static jfieldID und_attrs_st_mtime_sec;
static jfieldID und_attrs_st_mtime_nsec;
static jfieldID und_attrs_st_ctime_sec;
static jfieldID und_attrs_st_ctime_nsec;
static jfieldID und_attrs_st_birthtime_sec;

static jbyteArray UnixNativeDispatcher_newByteArray(JNIEnv* env, const char* value) {
    if (!value) return NULL;
    size_t len = strlen(value);
    jbyteArray result = (*env)->NewByteArray(env, (jsize)len);
    if (result != NULL && len > 0) {
        (*env)->SetByteArrayRegion(env, result, 0, (jsize)len, (const jbyte*)value);
    }
    return result;
}

static void UnixNativeDispatcher_throwUnixException(JNIEnv* env, int errnum) {
    jclass cls = (*env)->FindClass(env, "sun/nio/fs/UnixException");
    if (!cls) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        jclass ioCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioCls) (*env)->ThrowNew(env, ioCls, strerror(errnum));
        return;
    }
    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
    if (!ctor) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cls);
        jclass ioCls = (*env)->FindClass(env, "java/io/IOException");
        if (ioCls) (*env)->ThrowNew(env, ioCls, strerror(errnum));
        return;
    }
    jobject ex = (*env)->NewObject(env, cls, ctor, (jint)errnum);
    if (ex != NULL) {
        (*env)->Throw(env, ex);
    }
    (*env)->DeleteLocalRef(env, cls);
}

static jfieldID UnixNativeDispatcher_requiredField(JNIEnv* env,
                                                   jclass cls,
                                                   const char* name,
                                                   const char* sig) {
    jfieldID field = (*env)->GetFieldID(env, cls, name, sig);
    if (!field && (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    return field;
}

static int UnixNativeDispatcher_initAttributeFields(JNIEnv* env) {
    if (und_attrs_st_mode != NULL) return 1;
    jclass cls = (*env)->FindClass(env, "sun/nio/fs/UnixFileAttributes");
    if (!cls) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return 0;
    }
    und_attrs_st_mode = UnixNativeDispatcher_requiredField(env, cls, "st_mode", "I");
    und_attrs_st_ino = UnixNativeDispatcher_requiredField(env, cls, "st_ino", "J");
    und_attrs_st_dev = UnixNativeDispatcher_requiredField(env, cls, "st_dev", "J");
    und_attrs_st_rdev = UnixNativeDispatcher_requiredField(env, cls, "st_rdev", "J");
    und_attrs_st_nlink = UnixNativeDispatcher_requiredField(env, cls, "st_nlink", "I");
    und_attrs_st_uid = UnixNativeDispatcher_requiredField(env, cls, "st_uid", "I");
    und_attrs_st_gid = UnixNativeDispatcher_requiredField(env, cls, "st_gid", "I");
    und_attrs_st_size = UnixNativeDispatcher_requiredField(env, cls, "st_size", "J");
    und_attrs_st_atime_sec = UnixNativeDispatcher_requiredField(env, cls, "st_atime_sec", "J");
    und_attrs_st_atime_nsec = UnixNativeDispatcher_requiredField(env, cls, "st_atime_nsec", "J");
    und_attrs_st_mtime_sec = UnixNativeDispatcher_requiredField(env, cls, "st_mtime_sec", "J");
    und_attrs_st_mtime_nsec = UnixNativeDispatcher_requiredField(env, cls, "st_mtime_nsec", "J");
    und_attrs_st_ctime_sec = UnixNativeDispatcher_requiredField(env, cls, "st_ctime_sec", "J");
    und_attrs_st_ctime_nsec = UnixNativeDispatcher_requiredField(env, cls, "st_ctime_nsec", "J");
    und_attrs_st_birthtime_sec = UnixNativeDispatcher_requiredField(env, cls, "st_birthtime_sec", "J");
    (*env)->DeleteLocalRef(env, cls);
    return und_attrs_st_mode && und_attrs_st_ino && und_attrs_st_dev &&
           und_attrs_st_rdev && und_attrs_st_nlink && und_attrs_st_uid &&
           und_attrs_st_gid && und_attrs_st_size && und_attrs_st_atime_sec &&
           und_attrs_st_mtime_sec && und_attrs_st_ctime_sec;
}

static void UnixNativeDispatcher_fillAttributes(JNIEnv* env, jobject attrs, const struct stat* sb) {
    if (!attrs || !sb || !UnixNativeDispatcher_initAttributeFields(env)) return;
    (*env)->SetIntField(env, attrs, und_attrs_st_mode, (jint)sb->st_mode);
    (*env)->SetLongField(env, attrs, und_attrs_st_ino, (jlong)sb->st_ino);
    (*env)->SetLongField(env, attrs, und_attrs_st_dev, (jlong)sb->st_dev);
    (*env)->SetLongField(env, attrs, und_attrs_st_rdev, (jlong)sb->st_rdev);
    (*env)->SetIntField(env, attrs, und_attrs_st_nlink, (jint)sb->st_nlink);
    (*env)->SetIntField(env, attrs, und_attrs_st_uid, (jint)sb->st_uid);
    (*env)->SetIntField(env, attrs, und_attrs_st_gid, (jint)sb->st_gid);
    (*env)->SetLongField(env, attrs, und_attrs_st_size, (jlong)sb->st_size);
    (*env)->SetLongField(env, attrs, und_attrs_st_atime_sec, (jlong)sb->st_atime);
    (*env)->SetLongField(env, attrs, und_attrs_st_mtime_sec, (jlong)sb->st_mtime);
    (*env)->SetLongField(env, attrs, und_attrs_st_ctime_sec, (jlong)sb->st_ctime);
#if defined(__APPLE__)
    if (und_attrs_st_atime_nsec) (*env)->SetLongField(env, attrs, und_attrs_st_atime_nsec, (jlong)sb->st_atimespec.tv_nsec);
    if (und_attrs_st_mtime_nsec) (*env)->SetLongField(env, attrs, und_attrs_st_mtime_nsec, (jlong)sb->st_mtimespec.tv_nsec);
    if (und_attrs_st_ctime_nsec) (*env)->SetLongField(env, attrs, und_attrs_st_ctime_nsec, (jlong)sb->st_ctimespec.tv_nsec);
    if (und_attrs_st_birthtime_sec) (*env)->SetLongField(env, attrs, und_attrs_st_birthtime_sec, (jlong)sb->st_birthtimespec.tv_sec);
#else
    if (und_attrs_st_atime_nsec) (*env)->SetLongField(env, attrs, und_attrs_st_atime_nsec, (jlong)sb->st_atim.tv_nsec);
    if (und_attrs_st_mtime_nsec) (*env)->SetLongField(env, attrs, und_attrs_st_mtime_nsec, (jlong)sb->st_mtim.tv_nsec);
    if (und_attrs_st_ctime_nsec) (*env)->SetLongField(env, attrs, und_attrs_st_ctime_nsec, (jlong)sb->st_ctim.tv_nsec);
    if (und_attrs_st_birthtime_sec) (*env)->SetLongField(env, attrs, und_attrs_st_birthtime_sec, (jlong)sb->st_mtime);
#endif
}

static const char* UnixNativeDispatcher_path(jlong pathAddress) {
    return (const char*)(uintptr_t)pathAddress;
}

static jint UnixNativeDispatcher_init(JNIEnv* env, jclass clazz) {
    UnixNativeDispatcher_initAttributeFields(env);
    return 2 | 4; /* SUPPORTS_OPENAT | SUPPORTS_FUTIMES */
}

static jbyteArray UnixNativeDispatcher_getcwd(JNIEnv* env, jclass clazz) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL || cwd[0] != '/') {
        snprintf(cwd, sizeof(cwd), "/data/local/tmp/westlake");
    }
    static int log_count = 0;
    if (log_count < 12) {
        log_count++;
        fprintf(stderr, "[PFCUT] UnixNativeDispatcher.getcwd -> %s\n", cwd);
    }
    return UnixNativeDispatcher_newByteArray(env, cwd);
}

static jbyteArray UnixNativeDispatcher_strerror(JNIEnv* env, jclass clazz, jint errnum) {
    return UnixNativeDispatcher_newByteArray(env, strerror(errnum));
}

static jint UnixNativeDispatcher_dup(JNIEnv* env, jclass clazz, jint fd) {
    int rc = dup((int)fd);
    if (rc < 0) UnixNativeDispatcher_throwUnixException(env, errno);
    return (jint)rc;
}

static jint UnixNativeDispatcher_open0(JNIEnv* env, jclass clazz, jlong pathAddress, jint flags, jint mode) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    int fd = open(path, (int)flags, (mode_t)mode);
    if (fd < 0) UnixNativeDispatcher_throwUnixException(env, errno);
    return (jint)fd;
}

static void UnixNativeDispatcher_close(JNIEnv* env, jclass clazz, jint fd) {
    if (close((int)fd) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
    }
}

static jint UnixNativeDispatcher_read(JNIEnv* env, jclass clazz, jint fd, jlong buf, jint nbyte) {
    ssize_t rc = read((int)fd, (void*)(uintptr_t)buf, (size_t)nbyte);
    if (rc < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return -1;
    }
    return (jint)rc;
}

static jint UnixNativeDispatcher_write(JNIEnv* env, jclass clazz, jint fd, jlong buf, jint nbyte) {
    ssize_t rc = write((int)fd, (const void*)(uintptr_t)buf, (size_t)nbyte);
    if (rc < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return -1;
    }
    return (jint)rc;
}

static void UnixNativeDispatcher_stat0(JNIEnv* env, jclass clazz, jlong pathAddress, jobject attrs) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    struct stat sb;
    if (stat(path, &sb) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return;
    }
    UnixNativeDispatcher_fillAttributes(env, attrs, &sb);
}

static jint UnixNativeDispatcher_stat1(JNIEnv* env, jclass clazz, jlong pathAddress) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    struct stat sb;
    if (stat(path, &sb) < 0) return 0;
    return (jint)sb.st_mode;
}

static void UnixNativeDispatcher_lstat0(JNIEnv* env, jclass clazz, jlong pathAddress, jobject attrs) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    struct stat sb;
    if (lstat(path, &sb) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return;
    }
    UnixNativeDispatcher_fillAttributes(env, attrs, &sb);
}

static void UnixNativeDispatcher_fstat(JNIEnv* env, jclass clazz, jint fd, jobject attrs) {
    struct stat sb;
    if (fstat((int)fd, &sb) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return;
    }
    UnixNativeDispatcher_fillAttributes(env, attrs, &sb);
}

static void UnixNativeDispatcher_fstatat0(JNIEnv* env,
                                          jclass clazz,
                                          jint dfd,
                                          jlong pathAddress,
                                          jint flag,
                                          jobject attrs) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    struct stat sb;
    int rc = fstatat((int)dfd, path, &sb, (int)flag);
    if (rc < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return;
    }
    UnixNativeDispatcher_fillAttributes(env, attrs, &sb);
}

static void UnixNativeDispatcher_access0(JNIEnv* env, jclass clazz, jlong pathAddress, jint amode) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    if (access(path, (int)amode) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
    }
}

static jboolean UnixNativeDispatcher_exists0(JNIEnv* env, jclass clazz, jlong pathAddress) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    return access(path, F_OK) == 0 ? JNI_TRUE : JNI_FALSE;
}

static jbyteArray UnixNativeDispatcher_realpath0(JNIEnv* env, jclass clazz, jlong pathAddress) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    char resolved[PATH_MAX];
    if (realpath(path, resolved) == NULL) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return NULL;
    }
    return UnixNativeDispatcher_newByteArray(env, resolved);
}

static jbyteArray UnixNativeDispatcher_readlink0(JNIEnv* env, jclass clazz, jlong pathAddress) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    char target[PATH_MAX];
    ssize_t len = readlink(path, target, sizeof(target) - 1);
    if (len < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return NULL;
    }
    target[len] = '\0';
    return UnixNativeDispatcher_newByteArray(env, target);
}

static void UnixNativeDispatcher_mkdir0(JNIEnv* env, jclass clazz, jlong pathAddress, jint mode) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    if (mkdir(path, (mode_t)mode) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
    }
}

static void UnixNativeDispatcher_rmdir0(JNIEnv* env, jclass clazz, jlong pathAddress) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    if (rmdir(path) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
    }
}

static void UnixNativeDispatcher_unlink0(JNIEnv* env, jclass clazz, jlong pathAddress) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    if (unlink(path) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
    }
}

static void UnixNativeDispatcher_rename0(JNIEnv* env, jclass clazz, jlong fromAddress, jlong toAddress) {
    const char* from = UnixNativeDispatcher_path(fromAddress);
    const char* to = UnixNativeDispatcher_path(toAddress);
    if (rename(from, to) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
    }
}

static void UnixNativeDispatcher_chmod0(JNIEnv* env, jclass clazz, jlong pathAddress, jint mode) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    if (chmod(path, (mode_t)mode) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
    }
}

static void UnixNativeDispatcher_fchmod(JNIEnv* env, jclass clazz, jint fd, jint mode) {
    if (fchmod((int)fd, (mode_t)mode) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
    }
}

static jlong UnixNativeDispatcher_opendir0(JNIEnv* env, jclass clazz, jlong pathAddress) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    DIR* dir = opendir(path);
    if (!dir) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return 0;
    }
    return (jlong)(uintptr_t)dir;
}

static jlong UnixNativeDispatcher_fdopendir(JNIEnv* env, jclass clazz, jint dfd) {
    DIR* dir = fdopendir((int)dfd);
    if (!dir) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return 0;
    }
    return (jlong)(uintptr_t)dir;
}

static void UnixNativeDispatcher_closedir(JNIEnv* env, jclass clazz, jlong dir) {
    DIR* value = (DIR*)(uintptr_t)dir;
    if (value && closedir(value) < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
    }
}

static jbyteArray UnixNativeDispatcher_readdir(JNIEnv* env, jclass clazz, jlong dir) {
    DIR* value = (DIR*)(uintptr_t)dir;
    if (!value) return NULL;
    errno = 0;
    struct dirent* entry = readdir(value);
    if (!entry) {
        if (errno != 0) UnixNativeDispatcher_throwUnixException(env, errno);
        return NULL;
    }
    return UnixNativeDispatcher_newByteArray(env, entry->d_name);
}

static jlong UnixNativeDispatcher_pathconf0(JNIEnv* env, jclass clazz, jlong pathAddress, jint name) {
    const char* path = UnixNativeDispatcher_path(pathAddress);
    long rc = pathconf(path, (int)name);
    if (rc < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return -1;
    }
    return (jlong)rc;
}

static jlong UnixNativeDispatcher_fpathconf(JNIEnv* env, jclass clazz, jint fd, jint name) {
    long rc = fpathconf((int)fd, (int)name);
    if (rc < 0) {
        UnixNativeDispatcher_throwUnixException(env, errno);
        return -1;
    }
    return (jlong)rc;
}

/* ==================== java.io.FileOutputStream natives ==================== */

static void FileOutputStream_initIDs(JNIEnv* env, jclass clazz) { /* no-op */ }

static void FileOutputStream_open0(JNIEnv* env, jobject thiz, jstring jname, jboolean append) {
    if (!jname) return;
    const char* name = (*env)->GetStringUTFChars(env, jname, NULL);
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = open(name, flags, 0666);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    if (fd < 0) {
        jclass fnfCls = (*env)->FindClass(env, "java/io/FileNotFoundException");
        if (fnfCls) (*env)->ThrowNew(env, fnfCls, strerror(errno));
        return;
    }
    /* Set fd on the FileDescriptor field */
    jclass fosCls = (*env)->GetObjectClass(env, thiz);
    jfieldID fdField = (*env)->GetFieldID(env, fosCls, "fd", "Ljava/io/FileDescriptor;");
    if (!fdField) { close(fd); return; }
    jobject fdObj = (*env)->GetObjectField(env, thiz, fdField);
    if (!fdObj) { close(fd); return; }
    setChannelFd(env, fdObj, fd);
}

static void FileOutputStream_write(JNIEnv* env, jobject thiz, jint b, jboolean append) {
    jclass fosCls = (*env)->GetObjectClass(env, thiz);
    jfieldID fdField = (*env)->GetFieldID(env, fosCls, "fd", "Ljava/io/FileDescriptor;");
    if (!fdField) return;
    jobject fdObj = (*env)->GetObjectField(env, thiz, fdField);
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return;
    char c = (char)b;
    write(fd, &c, 1);
}

static void FileOutputStream_writeBytes(JNIEnv* env, jobject thiz,
                                          jbyteArray bytes, jint off, jint len, jboolean append) {
    jclass fosCls = (*env)->GetObjectClass(env, thiz);
    jfieldID fdField = (*env)->GetFieldID(env, fosCls, "fd", "Ljava/io/FileDescriptor;");
    if (!fdField) return;
    jobject fdObj = (*env)->GetObjectField(env, thiz, fdField);
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return;
    jbyte* buf = (*env)->GetByteArrayElements(env, bytes, NULL);
    if (!buf) return;
    write(fd, buf + off, len);
    (*env)->ReleaseByteArrayElements(env, bytes, buf, JNI_ABORT);
}

static void FileOutputStream_close0(JNIEnv* env, jobject thiz) {
    /* Closing handled by IoBridge/Os on ART -- no-op here */
}

/* ==================== java.io.FileInputStream natives ==================== */

static void FileInputStream_initIDs(JNIEnv* env, jclass clazz) { /* no-op */ }

static void FileInputStream_open0(JNIEnv* env, jobject thiz, jstring jname) {
    if (!jname) return;
    const char* name = (*env)->GetStringUTFChars(env, jname, NULL);
    int fd = open(name, O_RDONLY);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    if (fd < 0) {
        jclass fnfCls = (*env)->FindClass(env, "java/io/FileNotFoundException");
        if (fnfCls) (*env)->ThrowNew(env, fnfCls, strerror(errno));
        return;
    }
    jclass fisCls = (*env)->GetObjectClass(env, thiz);
    jfieldID fdField = (*env)->GetFieldID(env, fisCls, "fd", "Ljava/io/FileDescriptor;");
    if (!fdField) { close(fd); return; }
    jobject fdObj = (*env)->GetObjectField(env, thiz, fdField);
    if (!fdObj) { close(fd); return; }
    setChannelFd(env, fdObj, fd);
}

static jint FileInputStream_read0(JNIEnv* env, jobject thiz) {
    jclass fisCls = (*env)->GetObjectClass(env, thiz);
    jfieldID fdField = (*env)->GetFieldID(env, fisCls, "fd", "Ljava/io/FileDescriptor;");
    if (!fdField) return -1;
    jobject fdObj = (*env)->GetObjectField(env, thiz, fdField);
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    unsigned char c;
    ssize_t n = read(fd, &c, 1);
    return (n <= 0) ? -1 : (jint)c;
}

static jint FileInputStream_readBytes(JNIEnv* env, jobject thiz,
                                       jbyteArray bytes, jint off, jint len) {
    jclass fisCls = (*env)->GetObjectClass(env, thiz);
    jfieldID fdField = (*env)->GetFieldID(env, fisCls, "fd", "Ljava/io/FileDescriptor;");
    if (!fdField) return -1;
    jobject fdObj = (*env)->GetObjectField(env, thiz, fdField);
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return -1;
    jbyte* buf = (*env)->GetByteArrayElements(env, bytes, NULL);
    if (!buf) return -1;
    ssize_t n = read(fd, buf + off, len);
    (*env)->ReleaseByteArrayElements(env, bytes, buf, 0);
    return (n <= 0) ? -1 : (jint)n;
}

static jlong FileInputStream_skip0(JNIEnv* env, jobject thiz, jlong n) {
    jclass fisCls = (*env)->GetObjectClass(env, thiz);
    jfieldID fdField = (*env)->GetFieldID(env, fisCls, "fd", "Ljava/io/FileDescriptor;");
    if (!fdField) return 0;
    jobject fdObj = (*env)->GetObjectField(env, thiz, fdField);
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return 0;
    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur == (off_t)-1) return 0;
    off_t end = lseek(fd, (off_t)n, SEEK_CUR);
    if (end == (off_t)-1) return 0;
    return (jlong)(end - cur);
}

static jint FileInputStream_available0(JNIEnv* env, jobject thiz) {
    jclass fisCls = (*env)->GetObjectClass(env, thiz);
    jfieldID fdField = (*env)->GetFieldID(env, fisCls, "fd", "Ljava/io/FileDescriptor;");
    if (!fdField) return 0;
    jobject fdObj = (*env)->GetObjectField(env, thiz, fdField);
    int fd = getChannelFd(env, fdObj);
    if (fd < 0) return 0;
    struct stat sb;
    if (fstat(fd, &sb) < 0) return 0;
    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur == (off_t)-1) return 0;
    jint avail = (jint)(sb.st_size - cur);
    return (avail > 0) ? avail : 0;
}

static void FileInputStream_close0(JNIEnv* env, jobject thiz) {
    /* Closing handled by IoBridge/Os on ART -- no-op here */
}

/* ==================== java.io.UnixFileSystem natives ==================== */

/* Constants from java.io.FileSystem */
#define BA_EXISTS    0x01
#define BA_REGULAR   0x02
#define BA_DIRECTORY 0x04
#define BA_HIDDEN    0x08
#define ACCESS_EXECUTE 0x01
#define ACCESS_WRITE   0x02
#define ACCESS_READ    0x04

static jint UnixFileSystem_computeBooleanAttributes(const char* path) {
    struct stat sb;
    int rv = 0;
    if (path != NULL && stat(path, &sb) == 0) {
        int fmt = sb.st_mode & S_IFMT;
        rv = BA_EXISTS
            | ((fmt == S_IFREG) ? BA_REGULAR : 0)
            | ((fmt == S_IFDIR) ? BA_DIRECTORY : 0);
        const char* base = strrchr(path, '/');
        base = (base != NULL) ? base + 1 : path;
        if (base != NULL && base[0] == '.' && base[1] != '\0') {
            rv |= BA_HIDDEN;
        }
    }
    return rv;
}

/* WESTLAKE §639: a userspace pointer on this target has zero bits above 47. A value like
 * 0x69b8014056fd8 (garbage<<32 | heap-ref) fails this and would otherwise be dereferenced. */
#define WL_PTR_PLAUSIBLE(p) ((p) != NULL && ((uintptr_t)(p) >> 48) == 0)

static const char* File_getPathChars(JNIEnv* env, jobject file, jstring* out_jpath) {
    if (!WL_PTR_PLAUSIBLE(file)) return NULL;
    jclass fileCls = (*env)->GetObjectClass(env, file);
    if (!fileCls) return NULL;
    jmethodID getPath = (*env)->GetMethodID(env, fileCls, "getPath", "()Ljava/lang/String;");
    (*env)->DeleteLocalRef(env, fileCls);
    if (!getPath) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return NULL;
    }
    jstring jpath = (jstring)(*env)->CallObjectMethod(env, file, getPath);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    if (!jpath) return NULL;
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path && (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, jpath);
        return NULL;
    }
    /* WESTLAKE §639 (2026-08-15): validate the pointer before ANY caller uses it.
     * Toutiao faulted inside Westlake_UnixFileSystem_getLastModifiedTime at
     * addr=0x69b8014056fd8 — note the shape: garbage<<32 | 0x14056fd8, where the low half is a
     * plausible heap reference on this port. That is the §609 "int<<32 receiver" signature
     * (wide/reference vreg confusion), so one of `file` / `jpath` / `path` arrives with junk in
     * its upper 32 bits and the first strlen/stat on it walks off the map.
     * Every UnixFileSystem_* caller here already handles a NULL return by yielding its safe
     * default (0 / JNI_FALSE / NULL), so failing closed costs nothing — and the log names the
     * pointer that was wrong, which is the only cheap way to see it. */
    if (!WL_PTR_PLAUSIBLE(path)) {
        static int wl_bad_path = 0;
        if (wl_bad_path < 20) {
            wl_bad_path++;
            fprintf(stderr,
                    "[WESTLAKE-639] implausible UTF chars: file=%p jpath=%p path=%p\n",
                    (void*)file, (void*)jpath, (const void*)path);
            fflush(stderr);
        }
        if (path) (*env)->ReleaseStringUTFChars(env, jpath, path);
        (*env)->DeleteLocalRef(env, jpath);
        return NULL;
    }
    *out_jpath = jpath;
    return path;
}

static void File_releasePathChars(JNIEnv* env, jstring jpath, const char* path) {
    if (jpath && path) {
        (*env)->ReleaseStringUTFChars(env, jpath, path);
    }
    if (jpath) {
        (*env)->DeleteLocalRef(env, jpath);
    }
}

static void UnixFileSystem_initIDs(JNIEnv* env, jclass clazz) { /* no-op */ }

static jint UnixFileSystem_getBooleanAttributes0(JNIEnv* env, jobject thiz, jobject file) {
    jstring jpath = NULL;
    const char* path = File_getPathChars(env, file, &jpath);
    if (!path) return 0;
    int rv = UnixFileSystem_computeBooleanAttributes(path);
    File_releasePathChars(env, jpath, path);
    return rv;
}

static jstring UnixFileSystem_canonicalize0(JNIEnv* env, jobject thiz, jstring jpath) {
    if (!jpath) return NULL;
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    char resolved[PATH_MAX];
    char* result = realpath(path, resolved);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (result) {
        return (*env)->NewStringUTF(env, resolved);
    }
    /* If realpath fails, return the original path */
    return jpath;
}

static jlong UnixFileSystem_getLastModifiedTime0(JNIEnv* env, jobject thiz, jobject file) {
    jstring jpath = NULL;
    const char* path = File_getPathChars(env, file, &jpath);
    if (!path) return 0;
    struct stat sb;
    jlong result = 0;
    if (stat(path, &sb) == 0) {
        result = (jlong)sb.st_mtime * 1000LL;
    }
    File_releasePathChars(env, jpath, path);
    return result;
}

static jboolean UnixFileSystem_checkAccess0(JNIEnv* env, jobject thiz, jobject file, jint access_mode) {
    jstring jpath = NULL;
    const char* path = File_getPathChars(env, file, &jpath);
    if (!path) return JNI_FALSE;
    int mode = 0;
    if (access_mode == 0) {
        mode = F_OK;
    } else {
        if ((access_mode & ACCESS_READ) != 0) mode |= R_OK;
        if ((access_mode & ACCESS_WRITE) != 0) mode |= W_OK;
        if ((access_mode & ACCESS_EXECUTE) != 0) mode |= X_OK;
    }
    if (mode == 0) mode = F_OK;
    jboolean ok = (access(path, mode) == 0) ? JNI_TRUE : JNI_FALSE;
    File_releasePathChars(env, jpath, path);
    return ok;
}

static jlong UnixFileSystem_getLength0(JNIEnv* env, jobject thiz, jobject file) {
    jstring jpath = NULL;
    const char* path = File_getPathChars(env, file, &jpath);
    if (!path) return 0;
    struct stat sb;
    jlong result = 0;
    if (stat(path, &sb) == 0) {
        result = (jlong)sb.st_size;
    }
    File_releasePathChars(env, jpath, path);
    return result;
}

static jboolean UnixFileSystem_createFileExclusively0(JNIEnv* env, jobject thiz, jstring jpath) {
    if (!jpath) return JNI_FALSE;
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (fd >= 0) {
        close(fd);
        return JNI_TRUE;
    }
    return JNI_FALSE;
}

static jboolean UnixFileSystem_createDirectory0(JNIEnv* env, jobject thiz, jobject file) {
    jstring jpath = NULL;
    const char* path = File_getPathChars(env, file, &jpath);
    if (!path) return JNI_FALSE;
    int result = mkdir(path, 0777);
    File_releasePathChars(env, jpath, path);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

static jobjectArray UnixFileSystem_list0(JNIEnv* env, jobject thiz, jobject file) {
    jstring jpath = NULL;
    const char* path = File_getPathChars(env, file, &jpath);
    if (!path) return NULL;
    DIR* dir = opendir(path);
    File_releasePathChars(env, jpath, path);
    if (!dir) return NULL;

    /* Count entries first */
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            count++;
    }
    rewinddir(dir);

    jclass stringCls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray result = (*env)->NewObjectArray(env, count, stringCls, NULL);
    int i = 0;
    while ((entry = readdir(dir)) != NULL && i < count) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            (*env)->SetObjectArrayElement(env, result, i++, (*env)->NewStringUTF(env, entry->d_name));
        }
    }
    closedir(dir);
    (*env)->DeleteLocalRef(env, stringCls);
    return result;
}

static jboolean UnixFileSystem_setPermission0(JNIEnv* env, jobject thiz, jobject file,
                                                jint access, jboolean enable, jboolean owneronly) {
    return JNI_FALSE; /* stub */
}

static jboolean UnixFileSystem_setLastModifiedTime0(JNIEnv* env, jobject thiz, jobject file, jlong time) {
    return JNI_FALSE; /* stub */
}

static jboolean UnixFileSystem_setReadOnly0(JNIEnv* env, jobject thiz, jobject file) {
    return JNI_FALSE; /* stub */
}

static jboolean UnixFileSystem_delete0(JNIEnv* env, jobject thiz, jobject file) {
    jstring jpath = NULL;
    const char* path = File_getPathChars(env, file, &jpath);
    if (!path) return JNI_FALSE;
    struct stat sb;
    int rc;
    if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        rc = rmdir(path);
    } else {
        rc = unlink(path);
    }
    File_releasePathChars(env, jpath, path);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

static jboolean UnixFileSystem_rename0(JNIEnv* env, jobject thiz, jobject from, jobject to) {
    jstring jfrom = NULL;
    jstring jto = NULL;
    const char* from_path = File_getPathChars(env, from, &jfrom);
    const char* to_path = File_getPathChars(env, to, &jto);
    if (!from_path || !to_path) {
        File_releasePathChars(env, jfrom, from_path);
        File_releasePathChars(env, jto, to_path);
        return JNI_FALSE;
    }
    int rc = rename(from_path, to_path);
    File_releasePathChars(env, jfrom, from_path);
    File_releasePathChars(env, jto, to_path);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

static jstring UnixFileSystem_parentOrNull(JNIEnv* env, jobject thiz, jstring jpath) {
    return NULL; /* ART will use Java fallback */
}

static jlong UnixFileSystem_getSpace0(JNIEnv* env, jobject thiz, jobject file, jint t) {
    return 0; /* stub */
}

static jlong UnixFileSystem_getNameMax0(JNIEnv* env, jobject thiz, jstring jpath) {
    const char* path = "/";
    if (jpath != NULL) {
        const char* candidate = (*env)->GetStringUTFChars(env, jpath, NULL);
        if (candidate != NULL) {
            path = candidate;
        } else if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
    }
    long result = pathconf(path, _PC_NAME_MAX);
    if (jpath != NULL && path != NULL && path != (const char*)"/") {
        (*env)->ReleaseStringUTFChars(env, jpath, path);
    }
    if (result < 0) result = NAME_MAX;
    return (jlong)result;
}

/* WESTLAKE §641 (2026-08-15): STOP INFERRING, MEASURE.
 * These wrappers fault on their very first JNI call (`ldr x8,[x0]; ldr x8,[x8,#248]`), and reading
 * the registers post-mortem left me unsure what x0 even is: it looked like a STACK address, and x1
 * looked like a pointer into libart's mapping rather than an ART IndirectRef. Rather than keep
 * guessing at the calling convention from disassembly, log exactly what the shim is handed.
 * Reading `*env` is safe here (env itself was readable; the fault was one level deeper, on
 * `(*env)[GetObjectClass]`), so we can test the function table pointer before using it.
 * Bailing returns each caller's existing safe default. */
static int wl_env_usable(JNIEnv* env, jobject thiz, jobject file, const char* who) {
    if (WL_PTR_PLAUSIBLE(env)) {
        const void* tbl = *(void* const*)env;
        if (WL_PTR_PLAUSIBLE(tbl)) {
            return 1;
        }
        static int wl_bad_tbl = 0;
        if (wl_bad_tbl < 20) {
            wl_bad_tbl++;
            /* WESTLAKE §642: name the CALLER from the frames themselves. The CHILDSEGV
             * backtracer claimed art_quick_generic_jni_trampoline, but codex established that
             * path always passes a real self->GetJniEnv(), and its own frame 2 return address
             * was not 4-byte aligned — so it was lying. __builtin_return_address walks the real
             * frame chain: ra0 = inside this shim's wrapper, ra1 = whoever invoked the wrapper. */
            fprintf(stderr,
                    "[WESTLAKE-641] %s: env=%p *env=%p thiz=%p file=%p (bad table) "
                    "ra0=%p ra1=%p slot0=0x%08x slot1=0x%08x\n",
                    who, (void*)env, (void*)tbl, (void*)thiz, (void*)file,
                    __builtin_return_address(0),
                    __builtin_return_address(1),
                    ((const unsigned*)env)[0], ((const unsigned*)env)[1]);
            fflush(stderr);
        }
        return 0;
    }
    {
        static int wl_bad_env = 0;
        if (wl_bad_env < 20) {
            wl_bad_env++;
            fprintf(stderr, "[WESTLAKE-641] %s: env=%p thiz=%p file=%p (bad env ptr)\n",
                    who, (void*)env, (void*)thiz, (void*)file);
            fflush(stderr);
        }
    }
    return 0;
}

/* Wrappers for public UnixFileSystem methods patched directly in runtime.cc. */
int Westlake_UnixFileSystem_getBooleanAttributes(JNIEnv* env, jobject thiz, jobject file) {
    if (!wl_env_usable(env, thiz, file, "getBooleanAttributes")) return 0;
    return UnixFileSystem_getBooleanAttributes0(env, thiz, file);
}

jboolean Westlake_UnixFileSystem_hasBooleanAttributes(JNIEnv* env, jobject thiz, jobject file, jint mask) {
    if (!wl_env_usable(env, thiz, file, "hasBooleanAttributes")) return JNI_FALSE;
    jint attrs = UnixFileSystem_getBooleanAttributes0(env, thiz, file);
    return ((attrs & mask) == mask) ? JNI_TRUE : JNI_FALSE;
}

jboolean Westlake_UnixFileSystem_checkAccess(JNIEnv* env, jobject thiz, jobject file, jint access_mode) {
    if (!wl_env_usable(env, thiz, file, "checkAccess")) return JNI_FALSE;
    return UnixFileSystem_checkAccess0(env, thiz, file, access_mode);
}

jlong Westlake_UnixFileSystem_getLastModifiedTime(JNIEnv* env, jobject thiz, jobject file) {
    if (!wl_env_usable(env, thiz, file, "getLastModifiedTime")) return 0;
    return UnixFileSystem_getLastModifiedTime0(env, thiz, file);
}

jlong Westlake_UnixFileSystem_getLength(JNIEnv* env, jobject thiz, jobject file) {
    if (!wl_env_usable(env, thiz, file, "getLength")) return 0;
    return UnixFileSystem_getLength0(env, thiz, file);
}

jobjectArray Westlake_UnixFileSystem_list(JNIEnv* env, jobject thiz, jobject file) {
    if (!wl_env_usable(env, thiz, file, "list")) return NULL;
    return UnixFileSystem_list0(env, thiz, file);
}

/* ==================== java.lang.Runtime natives ==================== */

/* Defined in patches/runtime/runtime.cc (extern "C"); linked into the same libart.so. */
extern int westlake_java_stack_contains(const char* needle);
extern void* _ZN3art6Thread14CurrentFromGdbEv(void);
extern jobject westlake_art_get_class_loader_override(void* thread);
extern void westlake_art_set_class_loader_override(void* thread, jobject class_loader);

/* WESTLAKE §754 (2026-08-20): opt-in diagnostics for the Android/OH native
 * library boundary.  Keep this observational: Android libraries must register
 * their own JNI surface through JNI_OnLoad rather than acquiring app-specific
 * native stubs in the framework. */
static int Westlake_trace_native_load(void) {
    const char* value = getenv("WESTLAKE_TRACE_NATIVE_LOAD");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 && strcmp(value, "FALSE") != 0 &&
           strcmp(value, "no") != 0 && strcmp(value, "NO") != 0 &&
           strcmp(value, "off") != 0 && strcmp(value, "OFF") != 0;
}

/* Opt-in Android native namespace boundary.
 *
 * OH's loader namespaces are the correct place to keep an app's Android
 * libc++/NDK ABI separate from the OH process namespace.  Direct targets and
 * the optional load-group anchor are supplied by the launcher so this runtime
 * contains no package or library special cases. */
typedef void (*WestlakeDlnsInitFn)(Dl_namespace*, const char*);
typedef int (*WestlakeDlnsGetFn)(const char*, Dl_namespace*);
typedef int (*WestlakeDlnsCreate2Fn)(Dl_namespace*, const char*, int);
typedef int (*WestlakeDlnsInheritFn)(Dl_namespace*, Dl_namespace*, const char*);
typedef void* (*WestlakeDlopenNsFn)(Dl_namespace*, const char*, int);

static pthread_mutex_t g_westlake_android_ns_mutex = PTHREAD_MUTEX_INITIALIZER;
static Dl_namespace g_westlake_android_ns;
static WestlakeDlopenNsFn g_westlake_dlopen_ns;
static int g_westlake_android_ns_state;

static int Westlake_native_basename_in_list(const char* path,
                                             const char* list) {
    if (path == NULL || list == NULL || list[0] == '\0') return 0;
    const char* basename = strrchr(path, '/');
    basename = basename != NULL ? basename + 1 : path;
    size_t basename_length = strlen(basename);
    const char* item = list;
    while (*item != '\0') {
        const char* end = strchr(item, ':');
        size_t item_length = end != NULL ? (size_t)(end - item) : strlen(item);
        if (item_length == basename_length &&
            memcmp(item, basename, basename_length) == 0) {
            return 1;
        }
        if (end == NULL) break;
        item = end + 1;
    }
    return 0;
}

static int Westlake_native_anchor_selected(const char* path) {
    return Westlake_native_basename_in_list(
            path, getenv("WESTLAKE_ANDROID_NATIVE_ANCHOR_TARGET"));
}

static int Westlake_native_direct_selected(const char* path) {
    return Westlake_native_basename_in_list(
            path, getenv("WESTLAKE_ANDROID_NATIVE_TARGETS"));
}

static void* Westlake_dlopen_android_boundary(const char* path,
                                               int use_anchor) {
    const char* search_path = getenv("WESTLAKE_ANDROID_NATIVE_SEARCH_PATH");
    const char* inherit_list = getenv("WESTLAKE_ANDROID_NATIVE_INHERIT");
    const char* open_path = path;
    if (use_anchor) {
        open_path = getenv("WESTLAKE_ANDROID_NATIVE_ANCHOR");
    }
    if (open_path == NULL || open_path[0] == '\0' ||
        search_path == NULL || search_path[0] == '\0') {
        fprintf(stderr,
                "[WESTLAKE-NATIVENS-761] missing load path or search path"
                " mode=%s\n",
                use_anchor ? "anchor" : "direct");
        return NULL;
    }
    if (inherit_list == NULL || inherit_list[0] == '\0') {
        inherit_list =
                "libc.so:libdl.so:libm.so:libz.so:libbionic_abi_shim.so";
    }

    pthread_mutex_lock(&g_westlake_android_ns_mutex);
    if (g_westlake_android_ns_state == 0) {
        WestlakeDlnsInitFn init_fn =
                (WestlakeDlnsInitFn)dlsym(RTLD_DEFAULT, "dlns_init");
        WestlakeDlnsGetFn get_fn =
                (WestlakeDlnsGetFn)dlsym(RTLD_DEFAULT, "dlns_get");
        WestlakeDlnsCreate2Fn create_fn =
                (WestlakeDlnsCreate2Fn)dlsym(RTLD_DEFAULT, "dlns_create2");
        WestlakeDlnsInheritFn inherit_fn =
                (WestlakeDlnsInheritFn)dlsym(RTLD_DEFAULT, "dlns_inherit");
        g_westlake_dlopen_ns =
                (WestlakeDlopenNsFn)dlsym(RTLD_DEFAULT, "dlopen_ns");
        Dl_namespace parent;
        memset(&parent, 0, sizeof(parent));
        memset(&g_westlake_android_ns, 0, sizeof(g_westlake_android_ns));
        int get_rc = get_fn != NULL ? get_fn(NULL, &parent) : -1;
        if (init_fn == NULL || create_fn == NULL || inherit_fn == NULL ||
            g_westlake_dlopen_ns == NULL || get_rc != 0) {
            g_westlake_android_ns_state = -1;
        } else {
            init_fn(&g_westlake_android_ns, "westlake_android_app");
            int create_rc = create_fn(&g_westlake_android_ns, search_path, 0);
            int inherit_rc = create_rc == 0
                    ? inherit_fn(&g_westlake_android_ns, &parent, inherit_list)
                    : -1;
            g_westlake_android_ns_state =
                    create_rc == 0 && inherit_rc == 0 ? 1 : -1;
            fprintf(stderr,
                    "[WESTLAKE-NATIVENS-761] create=%d inherit=%d state=%d"
                    " search=%s\n",
                    create_rc, inherit_rc, g_westlake_android_ns_state,
                    search_path);
        }
    }
    WestlakeDlopenNsFn open_fn = g_westlake_dlopen_ns;
    int ready = g_westlake_android_ns_state == 1;
    pthread_mutex_unlock(&g_westlake_android_ns_mutex);
    if (!ready || open_fn == NULL) return NULL;

    void* handle = open_fn(&g_westlake_android_ns, open_path,
                           RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr,
            "[WESTLAKE-NATIVENS-761] mode=%s requested=%s opened=%s"
            " handle=%p\n",
            use_anchor ? "anchor" : "direct",
            path != NULL ? path : "(null)", open_path, handle);
    fflush(stderr);
    return handle;
}

static void Runtime_nativeExit(JNIEnv* env, jclass clazz, jint status) {
    /* WESTLAKE (2026-07-11): sun.misc.Cleaner.clean() fires System.exit(1) when a
     * Cleaner thunk throws (restarted ReferenceQueueDaemon on this arm64 adapter).
     * A native-buffer cleanup failure must NOT terminate the app — swallow that
     * specific exit (Cleaner on the calling stack). All other exits proceed. */
    if (westlake_java_stack_contains("Cleaner")) {
        fprintf(stderr, "[EXIT-SWALLOW] Runtime_nativeExit(%d) suppressed — Cleaner thunk "
                "failure must not kill the app (returning to caller)\n", (int)status);
        fflush(stderr);
        return;
    }
    _exit(status);
}

static void Runtime_nativeGc(JNIEnv* env, jobject thiz) {
    /* no-op stub */
}

static jstring Runtime_nativeLoad(JNIEnv* env, jclass clazz, jstring filename,
                                   jobject classLoader, jclass caller) {
    if (!filename) return (*env)->NewStringUTF(env, "null filename");
    const char* path = (*env)->GetStringUTFChars(env, filename, NULL);
    fprintf(stderr, "[PF202N] Runtime_nativeLoad path=%s\n", path ? path : "(null)");
    fflush(stderr);
    /* Return success for statically-linked libraries */
    if (strstr(path, "javacore") || strstr(path, "openjdk") ||
        strstr(path, "icu_jni") || strstr(path, "icu-jni")) {
        fprintf(stderr, "[PF202N] Runtime_nativeLoad static-success path=%s\n", path);
        fflush(stderr);
        (*env)->ReleaseStringUTFChars(env, filename, path);
        return NULL; /* null = success, already registered */
    }
    /* OHBridge: register methods now (deferred from InitNativeMethods) */
    if (strstr(path, "oh_bridge")) {
        fprintf(stderr, "[PF202N] Runtime_nativeLoad oh_bridge dispatch path=%s\n", path);
        fflush(stderr);
        (*env)->ReleaseStringUTFChars(env, filename, path);
        JavaVM* vm; (*env)->GetJavaVM(env, &vm);
        JNI_OnLoad_ohbridge(vm, NULL);
        fprintf(stderr, "[PF202N] Runtime_nativeLoad oh_bridge returned\n");
        fflush(stderr);
        return NULL; /* null = success */
    }
    /*
     * android_runtime_stub: Westlake M3-finish.  The binder JNI methods
     * (Java_android_os_ServiceManager_native*) are statically linked into
     * dalvikvm via stubs/binder_jni_stub.cc.  Returning null = success makes
     * the System.loadLibrary("android_runtime_stub") in ServiceManager.java
     * succeed without invoking dlopen (which is a stub for bionic-static).
     *
     * Symbol resolution: ART's static build resolves Java natives via
     * dlsym(handle, "Java_*").  Since our handle is null (success), dlsym
     * never finds our static symbols — so we MUST RegisterNatives ourselves
     * via JNI_OnLoad_binder_with_cl, passing the calling classLoader so
     * FindClass("android/os/ServiceManager") works (the class lives in
     * aosp-shim.dex on the -cp, not bootclasspath).
     */
    if (strstr(path, "android_runtime_stub")) {
        fprintf(stderr, "[PF202N] Runtime_nativeLoad android_runtime_stub static-success path=%s\n", path);
        fflush(stderr);
        (*env)->ReleaseStringUTFChars(env, filename, path);
        extern jint JNI_OnLoad_binder_with_cl(JavaVM* vm, jobject classLoader);
        JavaVM* vm; (*env)->GetJavaVM(env, &vm);
        JNI_OnLoad_binder_with_cl(vm, classLoader);
        return NULL; /* null = success */
    }
    /*
     * Platform-first Realm boundary probe.
     *
     * The portable/static Westlake runtime cannot load stock APK shared
     * objects yet: bionic's static libdl path reports "libdl.a is a stub".
     * Returning success for Realm only lets Java initialization continue to
     * the interpreter-side Realm native-method defaults, so we can identify
     * the next real APK runtime blocker without claiming native .so support.
     */
    if (path && strstr(path, "librealm-jni") != NULL) {
        fprintf(stderr, "[PFCUT-REALM] Runtime_nativeLoad stub-success path=%s\n", path);
        fflush(stderr);
        (*env)->ReleaseStringUTFChars(env, filename, path);
        return NULL;
    }
    /* Try dlopen for other libraries.  Capture each libdl error immediately:
     * dlerror() consumes it, so a later diagnostic otherwise destroys the
     * failure text used by Runtime.load0(). */
    dlerror();
    const int use_android_anchor = Westlake_native_anchor_selected(path);
    const int use_android_direct = Westlake_native_direct_selected(path);
    void* handle = use_android_anchor || use_android_direct
            ? Westlake_dlopen_android_boundary(path, use_android_anchor)
            : dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    const char* open_error = dlerror();
    if (Westlake_trace_native_load()) {
        fprintf(stderr,
                "[WESTLAKE-NATIVELOAD-754] dlopen path=%s handle=%p error=%s\n",
                path ? path : "(null)", handle,
                open_error ? open_error : "(none)");
        fflush(stderr);
    }
    if (!handle) {
        jstring result = (*env)->NewStringUTF(
                env, open_error ? open_error : "dlopen not supported");
        (*env)->ReleaseStringUTFChars(env, filename, path);
        return result;
    }
    /* Call JNI_OnLoad if present */
    typedef jint (*JNI_OnLoad_fn)(JavaVM*, void*);
    dlerror();
    JNI_OnLoad_fn onLoad = (JNI_OnLoad_fn)dlsym(handle, "JNI_OnLoad");
    const char* symbol_error = dlerror();
    if (Westlake_trace_native_load()) {
        fprintf(stderr,
                "[WESTLAKE-NATIVELOAD-754] dlsym path=%s handle=%p "
                "JNI_OnLoad=%p error=%s\n",
                path ? path : "(null)", handle, (void*)onLoad,
                symbol_error ? symbol_error : "(none)");
        fflush(stderr);
    }
    if (onLoad) {
        JavaVM* vm;
        (*env)->GetJavaVM(env, &vm);
        void* art_thread = _ZN3art6Thread14CurrentFromGdbEv();
        jobject old_class_loader = NULL;
        if (art_thread != NULL) {
            old_class_loader = (*env)->NewLocalRef(
                    env, westlake_art_get_class_loader_override(art_thread));
            westlake_art_set_class_loader_override(art_thread, classLoader);
            fprintf(stderr,
                    "[PF202N] Runtime_nativeLoad class-loader override thread=%p loader=%p\n",
                    art_thread, classLoader);
            fflush(stderr);
        }
        jint ver = onLoad(vm, NULL);
        if (art_thread != NULL) {
            westlake_art_set_class_loader_override(art_thread, old_class_loader);
            if (old_class_loader != NULL) {
                (*env)->DeleteLocalRef(env, old_class_loader);
            }
        }
        if (ver < 0) {
            dlclose(handle);
            (*env)->ReleaseStringUTFChars(env, filename, path);
            return (*env)->NewStringUTF(env, "JNI_OnLoad returned error");
        }
    }
    (*env)->ReleaseStringUTFChars(env, filename, path);
    return NULL; /* null = success */
}

static jlong Runtime_freeMemory(JNIEnv* env, jobject thiz) {
    return 64 * 1024 * 1024; /* 64 MB */
}

static jlong Runtime_totalMemory(JNIEnv* env, jobject thiz) {
    return 256 * 1024 * 1024; /* 256 MB */
}

static jlong Runtime_maxMemory(JNIEnv* env, jobject thiz) {
    return 512 * 1024 * 1024; /* 512 MB */
}

/* java.lang.Runtime.runFinalization0 */
static void Runtime_runFinalization0(JNIEnv* env, jclass clazz) {
    /* no-op stub */
}

/* ==================== java.lang.Float / Double natives ==================== */

static jint Float_floatToRawIntBits(JNIEnv* env, jclass clazz, jfloat f) {
    union { jfloat f; jint i; } u;
    u.f = f;
    return u.i;
}

static jfloat Float_intBitsToFloat(JNIEnv* env, jclass clazz, jint i) {
    union { jfloat f; jint i; } u;
    u.i = i;
    return u.f;
}

static jlong Double_doubleToRawLongBits(JNIEnv* env, jclass clazz, jdouble d) {
    union { jdouble d; jlong l; } u;
    u.d = d;
    return u.l;
}

static jdouble Double_longBitsToDouble(JNIEnv* env, jclass clazz, jlong l) {
    union { jdouble d; jlong l; } u;
    u.l = l;
    return u.d;
}

/* ==================== JNI_OnLoad ==================== */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdint.h>

/* Minimal ZIP reading for java.util.zip.ZipFile */
/* We implement the subset ART's classloader needs */

/* ZIP file handle - just stores the fd and mmap */
typedef struct {
    int fd;
    uint8_t *data;
    size_t size;
    /* Central directory */
    uint8_t *cd_start;
    uint32_t cd_entries;
} NativeZipFile;

/* Find EOCD (End of Central Directory) */
static uint8_t* find_eocd(uint8_t* data, size_t size) {
    if (size < 22) return NULL;
    /* Search backwards from end */
    for (size_t i = size - 22; i > 0 && i > size - 65557; i--) {
        if (data[i] == 0x50 && data[i+1] == 0x4b && data[i+2] == 0x05 && data[i+3] == 0x06)
            return &data[i];
    }
    return NULL;
}

/* open(String name, int mode, long lastModified, boolean usemmap) -> long */
static jlong ZipFile_open(JNIEnv* env, jclass cls, jstring jname, jint mode, jlong lastMod, jboolean usemmap) {
    if (!jname) return 0;
    const char* name = (*env)->GetStringUTFChars(env, jname, NULL);
    int fd = open(name, O_RDONLY);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    if (fd < 0) return 0;
    
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return 0; }
    
    uint8_t* data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) { close(fd); return 0; }
    
    NativeZipFile* zf = calloc(1, sizeof(NativeZipFile));
    zf->fd = fd;
    zf->data = data;
    zf->size = st.st_size;
    
    /* Parse EOCD */
    uint8_t* eocd = find_eocd(data, st.st_size);
    if (eocd) {
        zf->cd_entries = eocd[8] | (eocd[9] << 8);
        uint32_t cd_offset = eocd[16] | (eocd[17] << 8) | (eocd[18] << 16) | (eocd[19] << 24);
        if (cd_offset < st.st_size) zf->cd_start = data + cd_offset;
    }
    
    return (jlong)(uintptr_t)zf;
}

/* close(long jzfile) */
static void ZipFile_close(JNIEnv* env, jclass cls, jlong handle) {
    NativeZipFile* zf = (NativeZipFile*)(uintptr_t)handle;
    if (!zf) return;
    if (zf->data) munmap(zf->data, zf->size);
    if (zf->fd >= 0) close(zf->fd);
    free(zf);
}

/* getTotal(long jzfile) -> int */
static jint ZipFile_getTotal(JNIEnv* env, jclass cls, jlong handle) {
    NativeZipFile* zf = (NativeZipFile*)(uintptr_t)handle;
    return zf ? (jint)zf->cd_entries : 0;
}

/* getEntry(long jzfile, byte[] name, boolean addSlash) -> long */
static jlong ZipFile_getEntry(JNIEnv* env, jclass cls, jlong handle, jbyteArray jname, jboolean addSlash) {
    NativeZipFile* zf = (NativeZipFile*)(uintptr_t)handle;
    if (!zf || !zf->cd_start || !jname) return 0;
    
    jsize nameLen = (*env)->GetArrayLength(env, jname);
    jbyte* nameBytes = (*env)->GetByteArrayElements(env, jname, NULL);
    
    /* Search central directory for matching entry */
    uint8_t* p = zf->cd_start;
    uint8_t* end = zf->data + zf->size;
    for (uint32_t i = 0; i < zf->cd_entries && p + 46 <= end; i++) {
        if (p[0] != 0x50 || p[1] != 0x4b || p[2] != 0x01 || p[3] != 0x02) break;
        uint16_t fnLen = p[28] | (p[29] << 8);
        uint16_t extraLen = p[30] | (p[31] << 8);
        uint16_t commentLen = p[32] | (p[33] << 8);
        
        if (fnLen == nameLen && p + 46 + fnLen <= end &&
            memcmp(p + 46, nameBytes, nameLen) == 0) {
            (*env)->ReleaseByteArrayElements(env, jname, nameBytes, JNI_ABORT);
            return (jlong)(uintptr_t)p; /* return pointer to CD entry as handle */
        }
        p += 46 + fnLen + extraLen + commentLen;
    }
    (*env)->ReleaseByteArrayElements(env, jname, nameBytes, JNI_ABORT);
    return 0;
}

/* getEntryBytes(long jzentry, int type) -> byte[] */
static jbyteArray ZipFile_getEntryBytes(JNIEnv* env, jclass cls, jlong entry, jint type) {
    uint8_t* p = (uint8_t*)(uintptr_t)entry;
    if (!p) return NULL;
    /* type: 0=name, 1=extra, 2=comment */
    uint16_t fnLen = p[28] | (p[29] << 8);
    if (type == 0) {
        jbyteArray result = (*env)->NewByteArray(env, fnLen);
        (*env)->SetByteArrayRegion(env, result, 0, fnLen, (jbyte*)(p + 46));
        return result;
    }
    return NULL;
}

/* getEntrySize(long jzentry) -> long (uncompressed size) */
static jlong ZipFile_getEntrySize(JNIEnv* env, jclass cls, jlong entry) {
    uint8_t* p = (uint8_t*)(uintptr_t)entry;
    if (!p) return -1;
    return p[24] | (p[25] << 8) | (p[26] << 16) | (p[27] << 24);
}

/* getEntryCSize(long jzentry) -> long (compressed size) */
static jlong ZipFile_getEntryCSize(JNIEnv* env, jclass cls, jlong entry) {
    uint8_t* p = (uint8_t*)(uintptr_t)entry;
    if (!p) return -1;
    return p[20] | (p[21] << 8) | (p[22] << 16) | (p[23] << 24);
}

/* getEntryMethod(long jzentry) -> int */
static jint ZipFile_getEntryMethod(JNIEnv* env, jclass cls, jlong entry) {
    uint8_t* p = (uint8_t*)(uintptr_t)entry;
    if (!p) return 0;
    return p[10] | (p[11] << 8);
}

/* getEntryTime(long jzentry) -> long */
static jlong ZipFile_getEntryTime(JNIEnv* env, jclass cls, jlong entry) {
    return 0; /* stub */
}

/* getEntryCrc(long jzentry) -> long */
static jlong ZipFile_getEntryCrc(JNIEnv* env, jclass cls, jlong entry) {
    uint8_t* p = (uint8_t*)(uintptr_t)entry;
    if (!p) return 0;
    return p[16] | (p[17] << 8) | (p[18] << 16) | (p[19] << 24);
}

/* getEntryFlag(long jzentry) -> int */
static jint ZipFile_getEntryFlag(JNIEnv* env, jclass cls, jlong entry) {
    uint8_t* p = (uint8_t*)(uintptr_t)entry;
    if (!p) return 0;
    return p[8] | (p[9] << 8);
}

/* getFileDescriptor(long jzfile) -> int */
static jint ZipFile_getFileDescriptor(JNIEnv* env, jclass cls, jlong handle) {
    NativeZipFile* zf = (NativeZipFile*)(uintptr_t)handle;
    return zf ? zf->fd : -1;
}

/* getCommentBytes(long jzfile) -> byte[] */
static jbyteArray ZipFile_getCommentBytes(JNIEnv* env, jclass cls, jlong handle) {
    return NULL;
}

/* read(long jzfile, long jzentry, long pos, byte[] b, int off, int len) -> int */
static jint ZipFile_read(JNIEnv* env, jclass cls, jlong handle, jlong entry, jlong pos,
                          jbyteArray buf, jint off, jint len) {
    NativeZipFile* zf = (NativeZipFile*)(uintptr_t)handle;
    uint8_t* p = (uint8_t*)(uintptr_t)entry;
    if (!zf || !p || !buf) return -1;
    
    /* Get local file header offset from CD entry */
    uint32_t localOff = p[42] | (p[43] << 8) | (p[44] << 16) | (p[45] << 24);
    if (localOff + 30 >= zf->size) return -1;
    
    /* Parse local file header */
    uint8_t* lh = zf->data + localOff;
    uint16_t lfnLen = lh[26] | (lh[27] << 8);
    uint16_t lextraLen = lh[28] | (lh[29] << 8);
    uint8_t* fileData = lh + 30 + lfnLen + lextraLen;
    
    uint32_t csize = p[20] | (p[21] << 8) | (p[22] << 16) | (p[23] << 24);
    if (pos >= csize) return -1;
    
    jint toRead = len;
    if (pos + toRead > csize) toRead = (jint)(csize - pos);
    
    (*env)->SetByteArrayRegion(env, buf, off, toRead, (jbyte*)(fileData + pos));
    return toRead;
}

/* Stubs for less critical methods */
static void ZipFile_ensureOpen(JNIEnv* env, jobject thiz) { }
static jobject ZipFile_getInflater(JNIEnv* env, jobject thiz) { return NULL; }
static void ZipFile_releaseInflater(JNIEnv* env, jobject thiz, jobject inflater) { }
static jobject ZipFile_getZipEntry(JNIEnv* env, jobject thiz, jstring name, jlong entry) { return NULL; }

static void ZipFile_freeEntry(JNIEnv* env, jclass cls, jlong handle, jlong entry) { /* no-op */ }

/* startsWithLOC - check if ZIP starts with local file header */
static jboolean ZipFile_startsWithLOC(JNIEnv* env, jclass cls, jlong handle) {
    NativeZipFile* zf = (NativeZipFile*)(uintptr_t)handle;
    if (!zf || zf->size < 4) return JNI_FALSE;
    return (zf->data[0] == 0x50 && zf->data[1] == 0x4b &&
            zf->data[2] == 0x03 && zf->data[3] == 0x04) ? JNI_TRUE : JNI_FALSE;
}
#include <math.h>

/* java.lang.Math native methods */
static jdouble Math_sin(JNIEnv* e, jclass c, jdouble a) { return sin(a); }
static jdouble Math_cos(JNIEnv* e, jclass c, jdouble a) { return cos(a); }
static jdouble Math_tan(JNIEnv* e, jclass c, jdouble a) { return tan(a); }
static jdouble Math_asin(JNIEnv* e, jclass c, jdouble a) { return asin(a); }
static jdouble Math_acos(JNIEnv* e, jclass c, jdouble a) { return acos(a); }
static jdouble Math_atan(JNIEnv* e, jclass c, jdouble a) { return atan(a); }
static jdouble Math_atan2(JNIEnv* e, jclass c, jdouble a, jdouble b) { return atan2(a, b); }
static jdouble Math_exp(JNIEnv* e, jclass c, jdouble a) { return exp(a); }
static jdouble Math_log(JNIEnv* e, jclass c, jdouble a) { return log(a); }
static jdouble Math_log10(JNIEnv* e, jclass c, jdouble a) { return log10(a); }
static jdouble Math_sqrt(JNIEnv* e, jclass c, jdouble a) { return sqrt(a); }
static jdouble Math_cbrt(JNIEnv* e, jclass c, jdouble a) { return cbrt(a); }
static jdouble Math_ceil(JNIEnv* e, jclass c, jdouble a) { return ceil(a); }
static jdouble Math_floor(JNIEnv* e, jclass c, jdouble a) { return floor(a); }
static jdouble Math_pow(JNIEnv* e, jclass c, jdouble a, jdouble b) { return pow(a, b); }
static jdouble Math_sinh(JNIEnv* e, jclass c, jdouble a) { return sinh(a); }
static jdouble Math_cosh(JNIEnv* e, jclass c, jdouble a) { return cosh(a); }
static jdouble Math_tanh(JNIEnv* e, jclass c, jdouble a) { return tanh(a); }
static jdouble Math_expm1(JNIEnv* e, jclass c, jdouble a) { return expm1(a); }
static jdouble Math_log1p(JNIEnv* e, jclass c, jdouble a) { return log1p(a); }
static jdouble Math_IEEEremainder(JNIEnv* e, jclass c, jdouble a, jdouble b) { return remainder(a, b); }
static jdouble Math_hypot(JNIEnv* e, jclass c, jdouble a, jdouble b) { return hypot(a, b); }
static jdouble Math_abs_d(JNIEnv* e, jclass c, jdouble a) { return fabs(a); }
static jdouble Math_max_d(JNIEnv* e, jclass c, jdouble a, jdouble b) { return fmax(a, b); }
static jdouble Math_min_d(JNIEnv* e, jclass c, jdouble a, jdouble b) { return fmin(a, b); }
static jdouble Math_copySign_d(JNIEnv* e, jclass c, jdouble a, jdouble b) { return copysign(a, b); }
static jdouble Math_toDegrees(JNIEnv* e, jclass c, jdouble a) { return a * (180.0 / 3.14159265358979323846); }
static jdouble Math_toRadians(JNIEnv* e, jclass c, jdouble a) { return a * (3.14159265358979323846 / 180.0); }
static jdouble Math_random(JNIEnv* e, jclass c) { return (double)rand() / RAND_MAX; }
static jdouble Math_rint(JNIEnv* e, jclass c, jdouble a) { return rint(a); }
static jint Math_round_f(JNIEnv* e, jclass c, jfloat a) { return (jint)roundf(a); }
static jlong Math_round_d(JNIEnv* e, jclass c, jdouble a) { return (jlong)round(a); }

/* ==================== android.graphics.Typeface stubs ==================== */
static jlong Typeface_nativeGetReleaseFunc(JNIEnv* env, jclass cls) { return 0; }
static jlong Typeface_nativeCreateFromTypeface(JNIEnv* env, jclass cls, jlong ni, jint style) { return ni ? ni : 1; }
static jlong Typeface_nativeCreateFromTypefaceWithExactStyle(JNIEnv* env, jclass cls, jlong ni, jint weight, jboolean italic) { return ni ? ni : 1; }
static jlong Typeface_nativeCreateWeightAlias(JNIEnv* env, jclass cls, jlong ni, jint weight) { return ni ? ni : 1; }
static jlong Typeface_nativeCreateFromArray(JNIEnv* env, jclass cls, jlongArray familyArray, jint weight, jint italic) { return 1; }
static jintArray Typeface_nativeGetSupportedAxes(JNIEnv* env, jclass cls, jlong ni) { return NULL; }
static void Typeface_nativeSetDefault(JNIEnv* env, jclass cls, jlong nativePtr) {}
static jint Typeface_nativeGetStyle(JNIEnv* env, jclass cls, jlong nativePtr) { return 0; }
static jint Typeface_nativeGetWeight(JNIEnv* env, jclass cls, jlong nativePtr) { return 400; }
static void Typeface_nativeRegisterGenericFamily(JNIEnv* env, jclass cls, jstring str, jlong nativePtr) {}

/* Forward declare ohbridge JNI_OnLoad - we call it ourselves since the weak link may not */
extern jint JNI_OnLoad_ohbridge(JavaVM* vm, void* reserved);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return -1;

    /* java.lang.System */
    {
        jclass cls = (*env)->FindClass(env, "java/lang/System");
        if (cls) {
            JNINativeMethod methods[] = {
                {"specialProperties", "()[Ljava/lang/String;", (void*)System_specialProperties},
                {"mapLibraryName", "(Ljava/lang/String;)Ljava/lang/String;", (void*)System_mapLibraryName},
                {"setErr0", "(Ljava/io/PrintStream;)V", (void*)System_setErr0},
                {"setOut0", "(Ljava/io/PrintStream;)V", (void*)System_setOut0},
                {"setIn0", "(Ljava/io/InputStream;)V", (void*)System_setIn0},
                {"log", "(CLjava/lang/String;Ljava/lang/Throwable;)V", (void*)System_log},
                {"nanoTime", "()J", (void*)System_nanoTime},
                {"currentTimeMillis", "()J", (void*)System_currentTimeMillis},
            };
            registerNativesOrSkip(env, cls, methods, 8);
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* sun.misc.Version */
    {
        jclass cls = (*env)->FindClass(env, "sun/misc/Version");
        if (cls) {
            JNINativeMethod methods[] = {
                {"getJvmSpecialVersion", "()Ljava/lang/String;", (void*)Version_getJvmSpecialVersion},
                {"getJdkSpecialVersion", "()Ljava/lang/String;", (void*)Version_getJdkSpecialVersion},
                {"getJvmVersionInfo", "()Z", (void*)Version_getJvmVersionInfo},
                {"getJdkVersionInfo", "()V", (void*)Version_getJdkVersionInfo},
            };
            registerNativesOrSkip(env, cls, methods, 4);
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.io.FileDescriptor */
    {
        jclass cls = (*env)->FindClass(env, "java/io/FileDescriptor");
        if (cls) {
            JNINativeMethod methods[] = {
                {"isSocket", "(I)Z", (void*)FileDescriptor_isSocket},
                {"getAppend", "(I)Z", (void*)FileDescriptor_getAppend},
                {"sync", "()V", (void*)FileDescriptor_sync},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* sun.nio.ch.FileDispatcherImpl */
    {
        jclass cls = (*env)->FindClass(env, "sun/nio/ch/FileDispatcherImpl");
        if (cls) {
            JNINativeMethod methods[] = {
                {"init0", "()V", (void*)FileDispatcherImpl_init},
                {"read0", "(Ljava/io/FileDescriptor;JI)I", (void*)FileDispatcherImpl_read0},
                {"write0", "(Ljava/io/FileDescriptor;JI)I", (void*)FileDispatcherImpl_write0},
                {"pread0", "(Ljava/io/FileDescriptor;JIJ)I", (void*)FileDispatcherImpl_pread0},
                {"pwrite0", "(Ljava/io/FileDescriptor;JIJ)I", (void*)FileDispatcherImpl_pwrite0},
                {"size0", "(Ljava/io/FileDescriptor;)J", (void*)FileDispatcherImpl_size0},
                {"close0", "(Ljava/io/FileDescriptor;)V", (void*)FileDispatcherImpl_close0},
                {"force0", "(Ljava/io/FileDescriptor;Z)I", (void*)FileDispatcherImpl_force0},
                {"seek0", "(Ljava/io/FileDescriptor;J)J", (void*)FileDispatcherImpl_seek0},
                {"truncate0", "(Ljava/io/FileDescriptor;J)I", (void*)FileDispatcherImpl_truncate0},
                {"lock0", "(Ljava/io/FileDescriptor;ZJJZ)I", (void*)FileDispatcherImpl_lock0},
                {"release0", "(Ljava/io/FileDescriptor;JJ)V", (void*)FileDispatcherImpl_release0},
                {"closeIntFD", "(I)V", (void*)FileDispatcherImpl_closeIntFD},
                {"allocationGranularity0", "()J", (void*)FileDispatcherImpl_allocationGranularity0},
                {"map0", "(Ljava/io/FileDescriptor;IJJZ)J", (void*)FileDispatcherImpl_map0},
                {"unmap0", "(JJ)I", (void*)FileChannelImpl_unmap0},
                {"setDirect0", "(Ljava/io/FileDescriptor;)I", (void*)FileDispatcherImpl_setDirect0},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* sun.nio.ch.UnixFileDispatcherImpl */
    {
        jclass cls = (*env)->FindClass(env, "sun/nio/ch/UnixFileDispatcherImpl");
        if (cls) {
            JNINativeMethod methods[] = {
                {"read0", "(Ljava/io/FileDescriptor;JI)I", (void*)FileDispatcherImpl_read0},
                {"write0", "(Ljava/io/FileDescriptor;JI)I", (void*)FileDispatcherImpl_write0},
                {"pread0", "(Ljava/io/FileDescriptor;JIJ)I", (void*)FileDispatcherImpl_pread0},
                {"pwrite0", "(Ljava/io/FileDescriptor;JIJ)I", (void*)FileDispatcherImpl_pwrite0},
                {"force0", "(Ljava/io/FileDescriptor;Z)I", (void*)FileDispatcherImpl_force0},
                {"seek0", "(Ljava/io/FileDescriptor;J)J", (void*)FileDispatcherImpl_seek0},
                {"truncate0", "(Ljava/io/FileDescriptor;J)I", (void*)FileDispatcherImpl_truncate0},
                {"size0", "(Ljava/io/FileDescriptor;)J", (void*)FileDispatcherImpl_size0},
                {"lock0", "(Ljava/io/FileDescriptor;ZJJZ)I", (void*)FileDispatcherImpl_lock0},
                {"release0", "(Ljava/io/FileDescriptor;JJ)V", (void*)FileDispatcherImpl_release0},
                {"closeIntFD", "(I)V", (void*)FileDispatcherImpl_closeIntFD},
                {"allocationGranularity0", "()J", (void*)FileDispatcherImpl_allocationGranularity0},
                {"map0", "(Ljava/io/FileDescriptor;IJJZ)J", (void*)FileDispatcherImpl_map0},
                {"unmap0", "(JJ)I", (void*)FileChannelImpl_unmap0},
                {"setDirect0", "(Ljava/io/FileDescriptor;)I", (void*)FileDispatcherImpl_setDirect0},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* sun.nio.ch.FileChannelImpl */
    {
        jclass cls = (*env)->FindClass(env, "sun/nio/ch/FileChannelImpl");
        if (cls) {
            JNINativeMethod methods[] = {
                {"initIDs", "()J", (void*)FileChannelImpl_initIDs},
                {"map0", "(IJJ)J", (void*)FileChannelImpl_map0},
                {"unmap0", "(JJ)I", (void*)FileChannelImpl_unmap0},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* sun.nio.ch.NativeThread */
    {
        jclass cls = (*env)->FindClass(env, "sun/nio/ch/NativeThread");
        if (cls) {
            JNINativeMethod methods[] = {
                {"current", "()J", (void*)NativeThread_current},
                {"signal", "(J)V", (void*)NativeThread_signal},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* sun.nio.fs.UnixNativeDispatcher */
    {
        jclass cls = (*env)->FindClass(env, "sun/nio/fs/UnixNativeDispatcher");
        if (cls) {
            JNINativeMethod methods[] = {
                {"init", "()I", (void*)UnixNativeDispatcher_init},
                {"getcwd", "()[B", (void*)UnixNativeDispatcher_getcwd},
                {"strerror", "(I)[B", (void*)UnixNativeDispatcher_strerror},
                {"dup", "(I)I", (void*)UnixNativeDispatcher_dup},
                {"open0", "(JII)I", (void*)UnixNativeDispatcher_open0},
                {"close", "(I)V", (void*)UnixNativeDispatcher_close},
                {"read", "(IJI)I", (void*)UnixNativeDispatcher_read},
                {"write", "(IJI)I", (void*)UnixNativeDispatcher_write},
                {"stat0", "(JLsun/nio/fs/UnixFileAttributes;)V", (void*)UnixNativeDispatcher_stat0},
                {"stat1", "(J)I", (void*)UnixNativeDispatcher_stat1},
                {"lstat0", "(JLsun/nio/fs/UnixFileAttributes;)V", (void*)UnixNativeDispatcher_lstat0},
                {"fstat", "(ILsun/nio/fs/UnixFileAttributes;)V", (void*)UnixNativeDispatcher_fstat},
                {"fstatat0", "(IJILsun/nio/fs/UnixFileAttributes;)V", (void*)UnixNativeDispatcher_fstatat0},
                {"access0", "(JI)V", (void*)UnixNativeDispatcher_access0},
                {"exists0", "(J)Z", (void*)UnixNativeDispatcher_exists0},
                {"realpath0", "(J)[B", (void*)UnixNativeDispatcher_realpath0},
                {"readlink0", "(J)[B", (void*)UnixNativeDispatcher_readlink0},
                {"mkdir0", "(JI)V", (void*)UnixNativeDispatcher_mkdir0},
                {"rmdir0", "(J)V", (void*)UnixNativeDispatcher_rmdir0},
                {"unlink0", "(J)V", (void*)UnixNativeDispatcher_unlink0},
                {"rename0", "(JJ)V", (void*)UnixNativeDispatcher_rename0},
                {"chmod0", "(JI)V", (void*)UnixNativeDispatcher_chmod0},
                {"fchmod", "(II)V", (void*)UnixNativeDispatcher_fchmod},
                {"opendir0", "(J)J", (void*)UnixNativeDispatcher_opendir0},
                {"fdopendir", "(I)J", (void*)UnixNativeDispatcher_fdopendir},
                {"closedir", "(J)V", (void*)UnixNativeDispatcher_closedir},
                {"readdir", "(J)[B", (void*)UnixNativeDispatcher_readdir},
                {"pathconf0", "(JI)J", (void*)UnixNativeDispatcher_pathconf0},
                {"fpathconf", "(II)J", (void*)UnixNativeDispatcher_fpathconf},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        } else {
            (*env)->ExceptionClear(env);
        }
    }

    /* java.io.FileOutputStream */
    {
        jclass cls = (*env)->FindClass(env, "java/io/FileOutputStream");
        if (cls) {
            JNINativeMethod methods[] = {
                {"open0", "(Ljava/lang/String;Z)V", (void*)FileOutputStream_open0},
                {"write", "(IZ)V", (void*)FileOutputStream_write},
                {"writeBytes", "([BIIZ)V", (void*)FileOutputStream_writeBytes},
                {"close0", "()V", (void*)FileOutputStream_close0},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.io.FileInputStream */
    {
        jclass cls = (*env)->FindClass(env, "java/io/FileInputStream");
        if (cls) {
            JNINativeMethod methods[] = {
                {"open0", "(Ljava/lang/String;)V", (void*)FileInputStream_open0},
                {"read0", "()I", (void*)FileInputStream_read0},
                {"readBytes", "([BII)I", (void*)FileInputStream_readBytes},
                {"skip0", "(J)J", (void*)FileInputStream_skip0},
                {"available0", "()I", (void*)FileInputStream_available0},
                {"close0", "()V", (void*)FileInputStream_close0},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.lang.Runtime */
    {
        jclass cls = (*env)->FindClass(env, "java/lang/Runtime");
        if (cls) {
            JNINativeMethod methods[] = {
                {"nativeExit", "(I)V", (void*)Runtime_nativeExit},
                {"nativeGc", "()V", (void*)Runtime_nativeGc},
                {"nativeLoad", "(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/Class;)Ljava/lang/String;", (void*)Runtime_nativeLoad},
                {"freeMemory", "()J", (void*)Runtime_freeMemory},
                {"totalMemory", "()J", (void*)Runtime_totalMemory},
                {"maxMemory", "()J", (void*)Runtime_maxMemory},
                {"runFinalization0", "()V", (void*)Runtime_runFinalization0},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.lang.Shutdown -- halt0 is the native that Runtime.halt() ultimately calls
       in some JDK/ART versions.  Register it defensively. */
    {
        jclass cls = (*env)->FindClass(env, "java/lang/Shutdown");
        if (cls) {
            JNINativeMethod methods[] = {
                {"halt0", "(I)V", (void*)Runtime_nativeExit},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        } else {
            (*env)->ExceptionClear(env); /* class may not exist in this DEX set */
        }
    }

    /* java.io.UnixFileSystem */
    {
        jclass cls = (*env)->FindClass(env, "java/io/UnixFileSystem");
        if (cls) {
            JNINativeMethod methods[] = {
                {"initIDs", "()V", (void*)UnixFileSystem_initIDs},
                {"getBooleanAttributes0", "(Ljava/io/File;)I", (void*)UnixFileSystem_getBooleanAttributes0},
                {"canonicalize0", "(Ljava/lang/String;)Ljava/lang/String;", (void*)UnixFileSystem_canonicalize0},
                {"canonicalize", "(Ljava/lang/String;)Ljava/lang/String;", (void*)UnixFileSystem_canonicalize0},
                {"parentOrNull", "(Ljava/lang/String;)Ljava/lang/String;", (void*)UnixFileSystem_parentOrNull},
                {"checkAccess0", "(Ljava/io/File;I)Z", (void*)UnixFileSystem_checkAccess0},
                {"getLastModifiedTime0", "(Ljava/io/File;)J", (void*)UnixFileSystem_getLastModifiedTime0},
                {"getLength0", "(Ljava/io/File;)J", (void*)UnixFileSystem_getLength0},
                {"createFileExclusively0", "(Ljava/lang/String;)Z", (void*)UnixFileSystem_createFileExclusively0},
                {"delete0", "(Ljava/io/File;)Z", (void*)UnixFileSystem_delete0},
                {"createDirectory0", "(Ljava/io/File;)Z", (void*)UnixFileSystem_createDirectory0},
                {"list0", "(Ljava/io/File;)[Ljava/lang/String;", (void*)UnixFileSystem_list0},
                {"rename0", "(Ljava/io/File;Ljava/io/File;)Z", (void*)UnixFileSystem_rename0},
                {"setPermission0", "(Ljava/io/File;IZZ)Z", (void*)UnixFileSystem_setPermission0},
                {"setLastModifiedTime0", "(Ljava/io/File;J)Z", (void*)UnixFileSystem_setLastModifiedTime0},
                {"setReadOnly0", "(Ljava/io/File;)Z", (void*)UnixFileSystem_setReadOnly0},
                {"getSpace0", "(Ljava/io/File;I)J", (void*)UnixFileSystem_getSpace0},
                {"getNameMax0", "(Ljava/lang/String;)J", (void*)UnixFileSystem_getNameMax0},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.util.zip.ZipFile */
    {
        jclass cls = (*env)->FindClass(env, "java/util/zip/ZipFile");
        if (cls) {
            JNINativeMethod methods[] = {
                {"open", "(Ljava/lang/String;IJZ)J", (void*)ZipFile_open},
                {"close", "(J)V", (void*)ZipFile_close},
                {"getTotal", "(J)I", (void*)ZipFile_getTotal},
                {"getEntry", "(J[BZ)J", (void*)ZipFile_getEntry},
                {"getEntryBytes", "(JI)[B", (void*)ZipFile_getEntryBytes},
                {"getEntrySize", "(J)J", (void*)ZipFile_getEntrySize},
                {"getEntryCSize", "(J)J", (void*)ZipFile_getEntryCSize},
                {"getEntryMethod", "(J)I", (void*)ZipFile_getEntryMethod},
                {"getEntryTime", "(J)J", (void*)ZipFile_getEntryTime},
                {"getEntryCrc", "(J)J", (void*)ZipFile_getEntryCrc},
                {"getEntryFlag", "(J)I", (void*)ZipFile_getEntryFlag},
                {"getFileDescriptor", "(J)I", (void*)ZipFile_getFileDescriptor},
                {"getCommentBytes", "(J)[B", (void*)ZipFile_getCommentBytes},
                {"read", "(JJJ[BII)I", (void*)ZipFile_read},
                {"freeEntry", "(JJ)V", (void*)ZipFile_freeEntry},
                {"startsWithLOC", "(J)Z", (void*)ZipFile_startsWithLOC},
                {"ensureOpen", "()V", (void*)ZipFile_ensureOpen},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }
    /* java.lang.Math */
    {
        jclass cls = (*env)->FindClass(env, "java/lang/Math");
        if (cls) {
            JNINativeMethod methods[] = {
                {"sin","(D)D",(void*)Math_sin},{"cos","(D)D",(void*)Math_cos},
                {"tan","(D)D",(void*)Math_tan},{"asin","(D)D",(void*)Math_asin},
                {"acos","(D)D",(void*)Math_acos},{"atan","(D)D",(void*)Math_atan},
                {"atan2","(DD)D",(void*)Math_atan2},{"exp","(D)D",(void*)Math_exp},
                {"log","(D)D",(void*)Math_log},{"log10","(D)D",(void*)Math_log10},
                {"sqrt","(D)D",(void*)Math_sqrt},{"cbrt","(D)D",(void*)Math_cbrt},
                {"ceil","(D)D",(void*)Math_ceil},{"floor","(D)D",(void*)Math_floor},
                {"pow","(DD)D",(void*)Math_pow},{"sinh","(D)D",(void*)Math_sinh},
                {"cosh","(D)D",(void*)Math_cosh},{"tanh","(D)D",(void*)Math_tanh},
                {"expm1","(D)D",(void*)Math_expm1},{"log1p","(D)D",(void*)Math_log1p},
                {"IEEEremainder","(DD)D",(void*)Math_IEEEremainder},
                {"hypot","(DD)D",(void*)Math_hypot},
                {"abs","(D)D",(void*)Math_abs_d},{"max","(DD)D",(void*)Math_max_d},
                {"copySign","(DD)D",(void*)Math_copySign_d},
                {"toDegrees","(D)D",(void*)Math_toDegrees},
                {"round","(F)I",(void*)Math_round_f},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }
    /* java.lang.StrictMath */
    {
        jclass cls = (*env)->FindClass(env, "java/lang/StrictMath");
        if (cls) {
            JNINativeMethod methods[] = {
                {"sin","(D)D",(void*)Math_sin},{"cos","(D)D",(void*)Math_cos},
                {"tan","(D)D",(void*)Math_tan},{"asin","(D)D",(void*)Math_asin},
                {"acos","(D)D",(void*)Math_acos},{"atan","(D)D",(void*)Math_atan},
                {"atan2","(DD)D",(void*)Math_atan2},{"exp","(D)D",(void*)Math_exp},
                {"log","(D)D",(void*)Math_log},{"log10","(D)D",(void*)Math_log10},
                {"sqrt","(D)D",(void*)Math_sqrt},{"cbrt","(D)D",(void*)Math_cbrt},
                {"ceil","(D)D",(void*)Math_ceil},{"floor","(D)D",(void*)Math_floor},
                {"pow","(DD)D",(void*)Math_pow},{"sinh","(D)D",(void*)Math_sinh},
                {"cosh","(D)D",(void*)Math_cosh},{"tanh","(D)D",(void*)Math_tanh},
                {"expm1","(D)D",(void*)Math_expm1},{"log1p","(D)D",(void*)Math_log1p},
                {"abs","(D)D",(void*)Math_abs_d},{"max","(DD)D",(void*)Math_max_d},
                {"toDegrees","(D)D",(void*)Math_toDegrees},
                {"random","()D",(void*)Math_random},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }
    /* java.lang.Float */
    {
        jclass cls = (*env)->FindClass(env, "java/lang/Float");
        if (cls) {
            JNINativeMethod methods[] = {
                {"floatToRawIntBits", "(F)I", (void*)Float_floatToRawIntBits},
                {"intBitsToFloat", "(I)F", (void*)Float_intBitsToFloat},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }
    /* java.lang.Double */
    {
        jclass cls = (*env)->FindClass(env, "java/lang/Double");
        if (cls) {
            JNINativeMethod methods[] = {
                {"doubleToRawLongBits", "(D)J", (void*)Double_doubleToRawLongBits},
                {"longBitsToDouble", "(J)D", (void*)Double_longBitsToDouble},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }
    /* android.graphics.Typeface */
    {
        jclass cls = (*env)->FindClass(env, "android/graphics/Typeface");
        if (cls) {
            JNINativeMethod methods[] = {
                {"nativeGetReleaseFunc", "()J", (void*)Typeface_nativeGetReleaseFunc},
                {"nativeCreateFromTypeface", "(JI)J", (void*)Typeface_nativeCreateFromTypeface},
                {"nativeCreateFromTypefaceWithExactStyle", "(JIZ)J", (void*)Typeface_nativeCreateFromTypefaceWithExactStyle},
                {"nativeCreateWeightAlias", "(JI)J", (void*)Typeface_nativeCreateWeightAlias},
                {"nativeCreateFromArray", "([JII)J", (void*)Typeface_nativeCreateFromArray},
                {"nativeGetSupportedAxes", "(J)[I", (void*)Typeface_nativeGetSupportedAxes},
                {"nativeSetDefault", "(J)V", (void*)Typeface_nativeSetDefault},
                {"nativeGetStyle", "(J)I", (void*)Typeface_nativeGetStyle},
                {"nativeGetWeight", "(J)I", (void*)Typeface_nativeGetWeight},
                {"nativeRegisterGenericFamily", "(Ljava/lang/String;J)V", (void*)Typeface_nativeRegisterGenericFamily},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* OHBridge registered later via Runtime_nativeLoad when System.loadLibrary is called */
    return JNI_VERSION_1_6;
}
