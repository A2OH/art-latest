#include <jni.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h>
#include <zlib.h>

#ifndef DEF_MEM_LEVEL
#define DEF_MEM_LEVEL 8
#endif

/* Tolerant RegisterNatives — skip methods that don't exist in this DEX version */
static void registerNativesOrSkip(JNIEnv* env, jclass clazz,
                                  const JNINativeMethod* methods, int numMethods) {
    for (int i = 0; i < numMethods; i++) {
        if ((*env)->RegisterNatives(env, clazz, &methods[i], 1) < 0) {
            (*env)->ExceptionClear(env);
        }
    }
}

/* ==================== Helper: get fd int from FileDescriptor object ==================== */

static int getFd(JNIEnv* env, jobject fdObj) {
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

static void setFd(JNIEnv* env, jobject fdObj, jint fd) {
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

static void throwErrnoException(JNIEnv* env, const char* functionName, int errnum) {
    jclass cls = (*env)->FindClass(env, "android/system/ErrnoException");
    if (!cls) return;
    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(Ljava/lang/String;I)V");
    if (!ctor) return;
    jstring name = (*env)->NewStringUTF(env, functionName);
    jobject exc = (*env)->NewObject(env, cls, ctor, name, (jint)errnum);
    if (exc) (*env)->Throw(env, (jthrowable)exc);
}

static void throwIOException(JNIEnv* env, const char* functionName, int errnum) {
    jclass cls = (*env)->FindClass(env, "java/io/IOException");
    if (!cls) return;
    char message[160];
    snprintf(message, sizeof(message), "%s failed: %s", functionName, strerror(errnum));
    (*env)->ThrowNew(env, cls, message);
}

static void throwOutOfMemoryError(JNIEnv* env, const char* message) {
    jclass cls = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
    if (cls) (*env)->ThrowNew(env, cls, message);
}

static void throwZipException(JNIEnv* env, const char* message) {
    jclass cls = (*env)->FindClass(env, "java/util/zip/ZipException");
    if (cls) (*env)->ThrowNew(env, cls, message);
}

static void throwDataFormatException(JNIEnv* env, const char* message) {
    jclass cls = (*env)->FindClass(env, "java/util/zip/DataFormatException");
    if (cls) (*env)->ThrowNew(env, cls, message);
}

/* ==================== libcore.io.Linux native methods ==================== */

/* getpwuid */
static jobject linux_getpwuid(JNIEnv* env, jobject thiz, jint uid) {
    struct passwd* pw = getpwuid(uid);
    const char* pw_name = pw ? pw->pw_name : "user";
    int pw_uid = pw ? pw->pw_uid : uid;
    int pw_gid = pw ? pw->pw_gid : uid;
    const char* pw_dir = pw ? pw->pw_dir : "/";
    const char* pw_shell = pw ? pw->pw_shell : "/bin/sh";

    jclass cls = (*env)->FindClass(env, "android/system/StructPasswd");
    if (!cls) return NULL;
    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>",
        "(Ljava/lang/String;IILjava/lang/String;Ljava/lang/String;)V");
    if (!ctor) return NULL;
    return (*env)->NewObject(env, cls, ctor,
        (*env)->NewStringUTF(env, pw_name),
        (jint)pw_uid, (jint)pw_gid,
        (*env)->NewStringUTF(env, pw_dir),
        (*env)->NewStringUTF(env, pw_shell));
}

/* uname */
static jobject linux_uname(JNIEnv* env, jobject thiz) {
    struct utsname buf;
    uname(&buf);
    jclass cls = (*env)->FindClass(env, "android/system/StructUtsname");
    if (!cls) return NULL;
    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (!ctor) return NULL;
    return (*env)->NewObject(env, cls, ctor,
        (*env)->NewStringUTF(env, buf.sysname),
        (*env)->NewStringUTF(env, buf.nodename),
        (*env)->NewStringUTF(env, buf.release),
        (*env)->NewStringUTF(env, buf.version),
        (*env)->NewStringUTF(env, buf.machine));
}

/* getenv */
static jstring linux_getenv(JNIEnv* env, jobject thiz, jstring name) {
    const char* n = (*env)->GetStringUTFChars(env, name, NULL);
    const char* val = getenv(n);
    (*env)->ReleaseStringUTFChars(env, name, n);
    return val ? (*env)->NewStringUTF(env, val) : NULL;
}

/* getuid / getpid / geteuid / getegid / getgid / getppid */
static jint linux_getuid(JNIEnv* env, jobject thiz) { return getuid(); }
static jint linux_getpid(JNIEnv* env, jobject thiz) { return getpid(); }
static jint linux_geteuid(JNIEnv* env, jobject thiz) { return geteuid(); }
static jint linux_getegid(JNIEnv* env, jobject thiz) { return getegid(); }
static jint linux_getgid(JNIEnv* env, jobject thiz) { return getgid(); }
static jint linux_getppid(JNIEnv* env, jobject thiz) { return getppid(); }

/* sysconf */
static jlong linux_sysconf(JNIEnv* env, jobject thiz, jint name) {
    /* Westlake: clamp processor count to 4 — bionic static sysconf returns garbage on some devices */
    long result = sysconf((int)name);
    if ((name == _SC_NPROCESSORS_CONF || name == _SC_NPROCESSORS_ONLN) && (result <= 0 || result > 64)) {
        result = 4;
    }
    return (jlong)result;
}

/* isatty */
static jboolean linux_isatty(JNIEnv* env, jobject thiz, jobject fdObj) {
    int fd = getFd(env, fdObj);
    return isatty(fd) ? JNI_TRUE : JNI_FALSE;
}

/* writeBytes(FileDescriptor fd, Object buffer, int offset, int byteCount) */
static jint linux_writeBytes(JNIEnv* env, jobject thiz,
                              jobject fdObj, jobject buffer, jint offset, jint byteCount) {
    int fd = getFd(env, fdObj);
    if (fd < 0) {
        throwErrnoException(env, "write", EBADF);
        return -1;
    }
    jbyteArray byteArray = (jbyteArray)buffer;
    jbyte* bytes = (*env)->GetByteArrayElements(env, byteArray, NULL);
    if (!bytes) return -1;
    ssize_t n = write(fd, bytes + offset, byteCount);
    (*env)->ReleaseByteArrayElements(env, byteArray, bytes, JNI_ABORT);
    if (n < 0) {
        throwErrnoException(env, "write", errno);
        return -1;
    }
    return (jint)n;
}

/* readBytes(FileDescriptor fd, Object buffer, int offset, int byteCount) */
static jint linux_readBytes(JNIEnv* env, jobject thiz,
                             jobject fdObj, jobject buffer, jint offset, jint byteCount) {
    int fd = getFd(env, fdObj);
    if (fd < 0) {
        throwErrnoException(env, "read", EBADF);
        return -1;
    }
    jbyteArray byteArray = (jbyteArray)buffer;
    jbyte* bytes = (*env)->GetByteArrayElements(env, byteArray, NULL);
    if (!bytes) return -1;
    ssize_t n = read(fd, bytes + offset, byteCount);
    (*env)->ReleaseByteArrayElements(env, byteArray, bytes, 0);
    if (n < 0) {
        throwErrnoException(env, "read", errno);
        return -1;
    }
    return (jint)n;
}

/* close(FileDescriptor fd) */
static void linux_close(JNIEnv* env, jobject thiz, jobject fdObj) {
    int fd = getFd(env, fdObj);
    if (fd >= 0) {
        close(fd);
    }
}

/* open(String path, int flags, int mode) -> FileDescriptor */
static jobject linux_open(JNIEnv* env, jobject thiz, jstring jpath, jint flags, jint mode) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    int fd = open(path, flags, mode);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (fd < 0) {
        throwErrnoException(env, "open", errno);
        return NULL;
    }
    jclass fdCls = (*env)->FindClass(env, "java/io/FileDescriptor");
    if (!fdCls) return NULL;
    jmethodID ctor = (*env)->GetMethodID(env, fdCls, "<init>", "()V");
    if (!ctor) return NULL;
    jobject fdObj = (*env)->NewObject(env, fdCls, ctor);
    if (!fdObj) return NULL;
    setFd(env, fdObj, fd);
    return fdObj;
}

/* lseek(FileDescriptor fd, long offset, int whence) */
static jlong linux_lseek(JNIEnv* env, jobject thiz, jobject fdObj, jlong offset, jint whence) {
    int fd = getFd(env, fdObj);
    off_t result = lseek(fd, (off_t)offset, whence);
    if (result == -1) {
        throwErrnoException(env, "lseek", errno);
        return -1;
    }
    return (jlong)result;
}

/* fstat(FileDescriptor fd) -> StructStat */
static jobject linux_fstat(JNIEnv* env, jobject thiz, jobject fdObj) {
    int fd = getFd(env, fdObj);
    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        throwErrnoException(env, "fstat", errno);
        return NULL;
    }
    jclass cls = (*env)->FindClass(env, "android/system/StructStat");
    if (!cls) return NULL;
    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(JJIJIIJJJJJJJ)V");
    if (!ctor) return NULL;
    return (*env)->NewObject(env, cls, ctor,
        (jlong)sb.st_dev, (jlong)sb.st_ino, (jint)sb.st_mode, (jlong)sb.st_nlink,
        (jint)sb.st_uid, (jint)sb.st_gid, (jlong)sb.st_rdev, (jlong)sb.st_size,
        (jlong)sb.st_atime, (jlong)sb.st_mtime, (jlong)sb.st_ctime,
        (jlong)sb.st_blksize, (jlong)sb.st_blocks);
}

/* sun.nio.ch.FileKey: key for shared FileChannel locks */
static void FileKey_initIDs(JNIEnv* env, jclass cls) {
    (void)env;
    (void)cls;
}

static void FileKey_init(JNIEnv* env, jobject thiz, jobject fdObj) {
    int fd = getFd(env, fdObj);
    if (fd < 0) {
        throwIOException(env, "fstat", EBADF);
        return;
    }

    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        throwIOException(env, "fstat", errno);
        return;
    }

    jclass cls = (*env)->GetObjectClass(env, thiz);
    jfieldID stDev = (*env)->GetFieldID(env, cls, "st_dev", "J");
    if (!stDev) return;
    jfieldID stIno = (*env)->GetFieldID(env, cls, "st_ino", "J");
    if (!stIno) return;
    (*env)->SetLongField(env, thiz, stDev, (jlong)sb.st_dev);
    (*env)->SetLongField(env, thiz, stIno, (jlong)sb.st_ino);
}

/* stat(String path) -> StructStat */
static jobject linux_stat(JNIEnv* env, jobject thiz, jstring jpath) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    struct stat sb;
    int result = stat(path, &sb);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (result < 0) {
        throwErrnoException(env, "stat", errno);
        return NULL;
    }
    jclass cls = (*env)->FindClass(env, "android/system/StructStat");
    if (!cls) return NULL;
    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(JJIJIIJJJJJJJ)V");
    if (!ctor) return NULL;
    return (*env)->NewObject(env, cls, ctor,
        (jlong)sb.st_dev, (jlong)sb.st_ino, (jint)sb.st_mode, (jlong)sb.st_nlink,
        (jint)sb.st_uid, (jint)sb.st_gid, (jlong)sb.st_rdev, (jlong)sb.st_size,
        (jlong)sb.st_atime, (jlong)sb.st_mtime, (jlong)sb.st_ctime,
        (jlong)sb.st_blksize, (jlong)sb.st_blocks);
}

/* lstat(String path) -> StructStat */
static jobject linux_lstat(JNIEnv* env, jobject thiz, jstring jpath) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    struct stat sb;
    int result = lstat(path, &sb);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (result < 0) {
        throwErrnoException(env, "lstat", errno);
        return NULL;
    }
    jclass cls = (*env)->FindClass(env, "android/system/StructStat");
    if (!cls) return NULL;
    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(JJIJIIJJJJJJJ)V");
    if (!ctor) return NULL;
    return (*env)->NewObject(env, cls, ctor,
        (jlong)sb.st_dev, (jlong)sb.st_ino, (jint)sb.st_mode, (jlong)sb.st_nlink,
        (jint)sb.st_uid, (jint)sb.st_gid, (jlong)sb.st_rdev, (jlong)sb.st_size,
        (jlong)sb.st_atime, (jlong)sb.st_mtime, (jlong)sb.st_ctime,
        (jlong)sb.st_blksize, (jlong)sb.st_blocks);
}

/* access(String path, int mode) */
static jboolean linux_access(JNIEnv* env, jobject thiz, jstring jpath, jint mode) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    int result = access(path, mode);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (result < 0) {
        throwErrnoException(env, "access", errno);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

/* environ() -> String[] */
static jobjectArray linux_environ(JNIEnv* env, jobject thiz) {
    extern char** environ;
    int count = 0;
    while (environ[count]) count++;
    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    jobjectArray result = (*env)->NewObjectArray(env, count, stringClass, NULL);
    for (int i = 0; i < count; i++) {
        (*env)->SetObjectArrayElement(env, result, i, (*env)->NewStringUTF(env, environ[i]));
    }
    return result;
}

/* setenv(String name, String value, boolean overwrite) */
static void linux_setenv(JNIEnv* env, jobject thiz, jstring jname, jstring jvalue, jboolean overwrite) {
    const char* name = (*env)->GetStringUTFChars(env, jname, NULL);
    const char* value = (*env)->GetStringUTFChars(env, jvalue, NULL);
    setenv(name, value, overwrite ? 1 : 0);
    (*env)->ReleaseStringUTFChars(env, jvalue, value);
    (*env)->ReleaseStringUTFChars(env, jname, name);
}

/* strerror(int errno) */
static jstring linux_strerror(JNIEnv* env, jobject thiz, jint errnum) {
    return (*env)->NewStringUTF(env, strerror(errnum));
}

/* mkdir(String path, int mode) */
static void linux_mkdir(JNIEnv* env, jobject thiz, jstring jpath, jint mode) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (mkdir(path, mode) < 0) {
        throwErrnoException(env, "mkdir", errno);
    }
    (*env)->ReleaseStringUTFChars(env, jpath, path);
}

/* rename(String oldPath, String newPath) */
static void linux_rename(JNIEnv* env, jobject thiz, jstring jold, jstring jnew) {
    const char* oldp = (*env)->GetStringUTFChars(env, jold, NULL);
    const char* newp = (*env)->GetStringUTFChars(env, jnew, NULL);
    if (rename(oldp, newp) < 0) {
        throwErrnoException(env, "rename", errno);
    }
    (*env)->ReleaseStringUTFChars(env, jnew, newp);
    (*env)->ReleaseStringUTFChars(env, jold, oldp);
}

/* remove(String path) */
static void linux_remove(JNIEnv* env, jobject thiz, jstring jpath) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (remove(path) < 0) {
        throwErrnoException(env, "remove", errno);
    }
    (*env)->ReleaseStringUTFChars(env, jpath, path);
}

/* unlink(String path) */
static void linux_unlink(JNIEnv* env, jobject thiz, jstring jpath) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (unlink(path) < 0) {
        throwErrnoException(env, "unlink", errno);
    }
    (*env)->ReleaseStringUTFChars(env, jpath, path);
}

/* chmod(String path, int mode) */
static void linux_chmod(JNIEnv* env, jobject thiz, jstring jpath, jint mode) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (chmod(path, mode) < 0) {
        throwErrnoException(env, "chmod", errno);
    }
    (*env)->ReleaseStringUTFChars(env, jpath, path);
}

/* fchmod(FileDescriptor fd, int mode) */
static void linux_fchmod(JNIEnv* env, jobject thiz, jobject fdObj, jint mode) {
    int fd = getFd(env, fdObj);
    if (fchmod(fd, mode) < 0) {
        throwErrnoException(env, "fchmod", errno);
    }
}

/* umask(int mask) */
static jint linux_umask(JNIEnv* env, jobject thiz, jint mask) {
    return (jint)umask(mask);
}

/* dup(FileDescriptor fd) -> FileDescriptor */
static jobject linux_dup(JNIEnv* env, jobject thiz, jobject fdObj) {
    int fd = getFd(env, fdObj);
    int newfd = dup(fd);
    if (newfd < 0) {
        throwErrnoException(env, "dup", errno);
        return NULL;
    }
    jclass fdCls = (*env)->FindClass(env, "java/io/FileDescriptor");
    if (!fdCls) return NULL;
    jmethodID ctor = (*env)->GetMethodID(env, fdCls, "<init>", "()V");
    if (!ctor) return NULL;
    jobject newFdObj = (*env)->NewObject(env, fdCls, ctor);
    if (!newFdObj) return NULL;
    setFd(env, newFdObj, newfd);
    return newFdObj;
}

/* dup2(FileDescriptor oldFd, int newFd) -> FileDescriptor */
static jobject linux_dup2(JNIEnv* env, jobject thiz, jobject fdObj, jint newfd) {
    int fd = getFd(env, fdObj);
    int result = dup2(fd, newfd);
    if (result < 0) {
        throwErrnoException(env, "dup2", errno);
        return NULL;
    }
    jclass fdCls = (*env)->FindClass(env, "java/io/FileDescriptor");
    if (!fdCls) return NULL;
    jmethodID ctor = (*env)->GetMethodID(env, fdCls, "<init>", "()V");
    if (!ctor) return NULL;
    jobject newFdObj = (*env)->NewObject(env, fdCls, ctor);
    if (!newFdObj) return NULL;
    setFd(env, newFdObj, result);
    return newFdObj;
}

/* ftruncate(FileDescriptor fd, long length) */
static void linux_ftruncate(JNIEnv* env, jobject thiz, jobject fdObj, jlong length) {
    int fd = getFd(env, fdObj);
    if (ftruncate(fd, (off_t)length) < 0) {
        throwErrnoException(env, "ftruncate", errno);
    }
}

/* fsync(FileDescriptor fd) */
static void linux_fsync(JNIEnv* env, jobject thiz, jobject fdObj) {
    int fd = getFd(env, fdObj);
    if (fsync(fd) < 0) {
        throwErrnoException(env, "fsync", errno);
    }
}

/* fdatasync(FileDescriptor fd) */
static void linux_fdatasync(JNIEnv* env, jobject thiz, jobject fdObj) {
    int fd = getFd(env, fdObj);
    if (fdatasync(fd) < 0) {
        throwErrnoException(env, "fdatasync", errno);
    }
}

/* symlink(String oldPath, String newPath) */
static void linux_symlink(JNIEnv* env, jobject thiz, jstring jold, jstring jnew) {
    const char* oldp = (*env)->GetStringUTFChars(env, jold, NULL);
    const char* newp = (*env)->GetStringUTFChars(env, jnew, NULL);
    if (symlink(oldp, newp) < 0) {
        throwErrnoException(env, "symlink", errno);
    }
    (*env)->ReleaseStringUTFChars(env, jnew, newp);
    (*env)->ReleaseStringUTFChars(env, jold, oldp);
}

/* readlink(String path) -> String */
static jstring linux_readlink(JNIEnv* env, jobject thiz, jstring jpath) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    char buf[PATH_MAX];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (n < 0) {
        throwErrnoException(env, "readlink", errno);
        return NULL;
    }
    buf[n] = '\0';
    return (*env)->NewStringUTF(env, buf);
}

/* realpath(String path) -> String */
static jstring linux_realpath(JNIEnv* env, jobject thiz, jstring jpath) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    char resolved[PATH_MAX];
    char* result = realpath(path, resolved);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (!result) {
        throwErrnoException(env, "realpath", errno);
        return NULL;
    }
    return (*env)->NewStringUTF(env, resolved);
}

/* getxattr(String path, String name) -> byte[] -- stub: not supported */
static jbyteArray linux_getxattr(JNIEnv* env, jobject thiz, jstring jpath, jstring jname) {
    throwErrnoException(env, "getxattr", ENOTSUP);
    return NULL;
}

/* android_fdsan stubs */
static void linux_android_fdsan_exchange_owner_tag(JNIEnv* env, jobject thiz,
                                                     jobject fd, jlong prev, jlong next) { }
static jlong linux_android_fdsan_get_owner_tag(JNIEnv* env, jobject thiz, jobject fd) { return 0; }
static jstring linux_android_fdsan_get_tag_type(JNIEnv* env, jobject thiz, jlong tag) {
    return (*env)->NewStringUTF(env, "unknown");
}
static jlong linux_android_fdsan_get_tag_value(JNIEnv* env, jobject thiz, jlong tag) { return 0; }

/* fcntlVoid / fcntlInt */
static jint linux_fcntlVoid(JNIEnv* env, jobject thiz, jobject fdObj, jint cmd) {
    int fd = getFd(env, fdObj);
    int result = fcntl(fd, cmd);
    if (result < 0) {
        throwErrnoException(env, "fcntl", errno);
    }
    return result;
}

static jint linux_fcntlInt(JNIEnv* env, jobject thiz, jobject fdObj, jint cmd, jint arg) {
    int fd = getFd(env, fdObj);
    int result = fcntl(fd, cmd, arg);
    if (result < 0) {
        throwErrnoException(env, "fcntl", errno);
    }
    return result;
}

/* ==================== OsConstants native methods ==================== */

static void OsConstants_initConstants(JNIEnv* env, jclass clazz) {
    #define SET_INT(name, val) do { \
        jfieldID fid = (*env)->GetStaticFieldID(env, clazz, #name, "I"); \
        if (fid) { (*env)->SetStaticIntField(env, clazz, fid, val); } \
        else { (*env)->ExceptionClear(env); } \
    } while(0)

    SET_INT(O_RDONLY, O_RDONLY);
    SET_INT(O_WRONLY, O_WRONLY);
    SET_INT(O_RDWR, O_RDWR);
    SET_INT(O_CREAT, O_CREAT);
    SET_INT(O_TRUNC, O_TRUNC);
    SET_INT(O_APPEND, O_APPEND);
    SET_INT(O_EXCL, O_EXCL);
    SET_INT(O_NONBLOCK, O_NONBLOCK);
    SET_INT(O_CLOEXEC, O_CLOEXEC);

    SET_INT(ENOENT, ENOENT);
    SET_INT(EACCES, EACCES);
    SET_INT(EEXIST, EEXIST);
    SET_INT(EBADF, EBADF);
    SET_INT(EINVAL, EINVAL);
    SET_INT(EIO, EIO);
    SET_INT(EISDIR, EISDIR);
    SET_INT(ENOTEMPTY, ENOTEMPTY);
    SET_INT(ENOSPC, ENOSPC);
    SET_INT(EPERM, EPERM);
    SET_INT(EROFS, EROFS);
    SET_INT(ENOMEM, ENOMEM);
    SET_INT(ENOTDIR, ENOTDIR);
    SET_INT(ENAMETOOLONG, ENAMETOOLONG);
    SET_INT(EMFILE, EMFILE);
    SET_INT(ENFILE, ENFILE);
    SET_INT(EINTR, EINTR);
    SET_INT(EAGAIN, EAGAIN);
    SET_INT(EWOULDBLOCK, EWOULDBLOCK);
    SET_INT(ECONNREFUSED, ECONNREFUSED);
    SET_INT(ETIMEDOUT, ETIMEDOUT);
    SET_INT(ECONNRESET, ECONNRESET);
    SET_INT(EPIPE, EPIPE);
    SET_INT(ENOTSOCK, ENOTSOCK);

    SET_INT(S_IFMT, S_IFMT);
    SET_INT(S_IFREG, S_IFREG);
    SET_INT(S_IFDIR, S_IFDIR);
    SET_INT(S_IFLNK, S_IFLNK);
    SET_INT(S_IFCHR, S_IFCHR);
    SET_INT(S_IFBLK, S_IFBLK);
    SET_INT(S_IFIFO, S_IFIFO);
    SET_INT(S_IFSOCK, S_IFSOCK);
    SET_INT(S_ISUID, S_ISUID);
    SET_INT(S_ISGID, S_ISGID);
    SET_INT(S_IRUSR, S_IRUSR);
    SET_INT(S_IWUSR, S_IWUSR);
    SET_INT(S_IXUSR, S_IXUSR);
    SET_INT(S_IRGRP, S_IRGRP);
    SET_INT(S_IWGRP, S_IWGRP);
    SET_INT(S_IXGRP, S_IXGRP);
    SET_INT(S_IROTH, S_IROTH);
    SET_INT(S_IWOTH, S_IWOTH);
    SET_INT(S_IXOTH, S_IXOTH);

    SET_INT(F_OK, F_OK);
    SET_INT(R_OK, R_OK);
    SET_INT(W_OK, W_OK);
    SET_INT(X_OK, X_OK);

    SET_INT(SEEK_SET, SEEK_SET);
    SET_INT(SEEK_CUR, SEEK_CUR);
    SET_INT(SEEK_END, SEEK_END);

    SET_INT(F_GETFD, F_GETFD);
    SET_INT(F_SETFD, F_SETFD);
    SET_INT(F_GETFL, F_GETFL);
    SET_INT(F_SETFL, F_SETFL);
    SET_INT(FD_CLOEXEC, FD_CLOEXEC);

    SET_INT(STDIN_FILENO, STDIN_FILENO);
    SET_INT(STDOUT_FILENO, STDOUT_FILENO);
    SET_INT(STDERR_FILENO, STDERR_FILENO);

    #undef SET_INT
}

/* ==================== libcore.io.Memory stubs ==================== */

static jbyte Memory_peekByte(JNIEnv* env, jclass cls, jlong address) {
    return *(jbyte*)(uintptr_t)address;
}
static void Memory_pokeByte(JNIEnv* env, jclass cls, jlong address, jbyte value) {
    *(jbyte*)(uintptr_t)address = value;
}
static jint Memory_peekIntNative(JNIEnv* env, jclass cls, jlong address) {
    return *(jint*)(uintptr_t)address;
}
static void Memory_pokeIntNative(JNIEnv* env, jclass cls, jlong address, jint value) {
    *(jint*)(uintptr_t)address = value;
}
static jlong Memory_peekLongNative(JNIEnv* env, jclass cls, jlong address) {
    return *(jlong*)(uintptr_t)address;
}
static void Memory_pokeLongNative(JNIEnv* env, jclass cls, jlong address, jlong value) {
    *(jlong*)(uintptr_t)address = value;
}
static jshort Memory_peekShortNative(JNIEnv* env, jclass cls, jlong address) {
    return *(jshort*)(uintptr_t)address;
}
static void Memory_pokeShortNative(JNIEnv* env, jclass cls, jlong address, jshort value) {
    *(jshort*)(uintptr_t)address = value;
}
static void Memory_peekByteArray(JNIEnv* env, jclass cls, jlong address, jbyteArray dst, jint dstOffset, jint count) {
    jbyte* ptr = (jbyte*)(uintptr_t)address;
    (*env)->SetByteArrayRegion(env, dst, dstOffset, count, ptr);
}
static void Memory_peekCharArray(JNIEnv* env, jclass cls, jlong address, jcharArray dst, jint dstOffset, jint count, jboolean swap) {
    jchar* ptr = (jchar*)(uintptr_t)address;
    (*env)->SetCharArrayRegion(env, dst, dstOffset, count, ptr);
}
static void Memory_peekIntArray(JNIEnv* env, jclass cls, jlong address, jintArray dst, jint dstOffset, jint count, jboolean swap) {
    jint* ptr = (jint*)(uintptr_t)address;
    (*env)->SetIntArrayRegion(env, dst, dstOffset, count, ptr);
}
static void Memory_peekShortArray(JNIEnv* env, jclass cls, jlong address, jshortArray dst, jint dstOffset, jint count, jboolean swap) {
    jshort* ptr = (jshort*)(uintptr_t)address;
    (*env)->SetShortArrayRegion(env, dst, dstOffset, count, ptr);
}
static void Memory_peekLongArray(JNIEnv* env, jclass cls, jlong address, jlongArray dst, jint dstOffset, jint count, jboolean swap) {
    jlong* ptr = (jlong*)(uintptr_t)address;
    (*env)->SetLongArrayRegion(env, dst, dstOffset, count, ptr);
}
static void Memory_peekFloatArray(JNIEnv* env, jclass cls, jlong address, jfloatArray dst, jint dstOffset, jint count, jboolean swap) {
    jfloat* ptr = (jfloat*)(uintptr_t)address;
    (*env)->SetFloatArrayRegion(env, dst, dstOffset, count, ptr);
}
static void Memory_peekDoubleArray(JNIEnv* env, jclass cls, jlong address, jdoubleArray dst, jint dstOffset, jint count, jboolean swap) {
    jdouble* ptr = (jdouble*)(uintptr_t)address;
    (*env)->SetDoubleArrayRegion(env, dst, dstOffset, count, ptr);
}
static void Memory_memmove(JNIEnv* env, jclass cls, jobject dstObj, jint dstOff, jobject srcObj, jint srcOff, jlong count) {
    if (!dstObj || !srcObj || count <= 0) return;
    jbyte* dst = (jbyte*)(*env)->GetPrimitiveArrayCritical(env, dstObj, NULL);
    jbyte* src = (jbyte*)(*env)->GetPrimitiveArrayCritical(env, srcObj, NULL);
    if (dst && src) memmove(dst + dstOff, src + srcOff, (size_t)count);
    if (src) (*env)->ReleasePrimitiveArrayCritical(env, srcObj, src, JNI_ABORT);
    if (dst) (*env)->ReleasePrimitiveArrayCritical(env, dstObj, dst, 0);
}

/* ==================== NativeBN stubs (BigInteger/BigDecimal) ==================== */
/* Minimal BigNum using malloc'd long value + sign bit */
typedef struct { long val; int neg; } SimpleBN;
static void nativeBN_free(void* ptr) { if (ptr) free(ptr); }

static jlong NativeBN_getNativeFinalizer(JNIEnv* env, jclass cls) {
    return (jlong)(void*)nativeBN_free;
}
static jlong NativeBN_BN_new(JNIEnv* env, jclass cls) {
    SimpleBN* bn = (SimpleBN*)calloc(1, sizeof(SimpleBN));
    return (jlong)(uintptr_t)bn;
}
static void NativeBN_BN_free(JNIEnv* env, jclass cls, jlong a) {
    if (a) free((void*)(uintptr_t)a);
}
static void NativeBN_putLongInt(JNIEnv* env, jclass cls, jlong a, jlong dw) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (bn) { bn->val = dw < 0 ? -dw : dw; bn->neg = dw < 0 ? 1 : 0; }
}
static void NativeBN_putULongInt(JNIEnv* env, jclass cls, jlong a, jlong dw, jboolean neg) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (bn) { bn->val = dw; bn->neg = neg ? 1 : 0; }
}
static jlong NativeBN_longInt(JNIEnv* env, jclass cls, jlong a) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (!bn) return 0;
    return bn->neg ? -bn->val : bn->val;
}
static jint NativeBN_sign(JNIEnv* env, jclass cls, jlong a) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (!bn || bn->val == 0) return 0;
    return bn->neg ? -1 : 1;
}
static jint NativeBN_bitLength(JNIEnv* env, jclass cls, jlong a) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (!bn || bn->val == 0) return 0;
    long v = bn->val;
    int bits = 0;
    while (v > 0) { bits++; v >>= 1; }
    return bits;
}
static jint NativeBN_BN_cmp(JNIEnv* env, jclass cls, jlong a, jlong b) {
    SimpleBN* ba = (SimpleBN*)(uintptr_t)a;
    SimpleBN* bb = (SimpleBN*)(uintptr_t)b;
    if (!ba || !bb) return 0;
    long va = ba->neg ? -ba->val : ba->val;
    long vb = bb->neg ? -bb->val : bb->val;
    return va < vb ? -1 : va > vb ? 1 : 0;
}
static void NativeBN_BN_copy(JNIEnv* env, jclass cls, jlong to, jlong from) {
    SimpleBN* bt = (SimpleBN*)(uintptr_t)to;
    SimpleBN* bf = (SimpleBN*)(uintptr_t)from;
    if (bt && bf) { bt->val = bf->val; bt->neg = bf->neg; }
}
static jstring NativeBN_BN_bn2dec(JNIEnv* env, jclass cls, jlong a) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    char buf[32];
    if (!bn) { snprintf(buf, sizeof(buf), "0"); }
    else { snprintf(buf, sizeof(buf), "%s%ld", bn->neg ? "-" : "", bn->val); }
    return (*env)->NewStringUTF(env, buf);
}
static jstring NativeBN_BN_bn2hex(JNIEnv* env, jclass cls, jlong a) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    char buf[32];
    if (!bn) { snprintf(buf, sizeof(buf), "0"); }
    else { snprintf(buf, sizeof(buf), "%s%lX", bn->neg ? "-" : "", bn->val); }
    return (*env)->NewStringUTF(env, buf);
}
static jint NativeBN_BN_dec2bn(JNIEnv* env, jclass cls, jlong a, jstring str) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (!bn || !str) return 0;
    const char* s = (*env)->GetStringUTFChars(env, str, NULL);
    long v = atol(s);
    (*env)->ReleaseStringUTFChars(env, str, s);
    bn->neg = v < 0 ? 1 : 0;
    bn->val = v < 0 ? -v : v;
    return 1;
}
static jint NativeBN_BN_hex2bn(JNIEnv* env, jclass cls, jlong a, jstring str) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (!bn || !str) return 0;
    const char* s = (*env)->GetStringUTFChars(env, str, NULL);
    long v = strtol(s, NULL, 16);
    (*env)->ReleaseStringUTFChars(env, str, s);
    bn->neg = v < 0 ? 1 : 0;
    bn->val = v < 0 ? -v : v;
    return 1;
}
static void NativeBN_BN_set_negative(JNIEnv* env, jclass cls, jlong b, jint n) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)b;
    if (bn) bn->neg = n ? 1 : 0;
}
static jboolean NativeBN_BN_is_bit_set(JNIEnv* env, jclass cls, jlong a, jint n) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (!bn || n < 0 || n >= 64) return JNI_FALSE;
    return (bn->val >> n) & 1 ? JNI_TRUE : JNI_FALSE;
}
static void NativeBN_BN_shift(JNIEnv* env, jclass cls, jlong r, jlong a, jint n) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)r;
    SimpleBN* ba = (SimpleBN*)(uintptr_t)a;
    if (!br || !ba) return;
    br->neg = ba->neg;
    if (n >= 0) br->val = ba->val << n;
    else br->val = ba->val >> (-n);
}
static void NativeBN_BN_add_word(JNIEnv* env, jclass cls, jlong a, jint w) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (bn) bn->val += w;
}
static void NativeBN_BN_mul_word(JNIEnv* env, jclass cls, jlong a, jint w) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (bn) bn->val *= w;
}
static jint NativeBN_BN_mod_word(JNIEnv* env, jclass cls, jlong a, jint w) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (!bn || w == 0) return 0;
    return (jint)(bn->val % w);
}
static void NativeBN_BN_add(JNIEnv* env, jclass cls, jlong r, jlong a, jlong b) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)r;
    SimpleBN* ba = (SimpleBN*)(uintptr_t)a;
    SimpleBN* bb = (SimpleBN*)(uintptr_t)b;
    if (!br || !ba || !bb) return;
    long va = ba->neg ? -ba->val : ba->val;
    long vb = bb->neg ? -bb->val : bb->val;
    long vr = va + vb;
    br->neg = vr < 0 ? 1 : 0;
    br->val = vr < 0 ? -vr : vr;
}
/* Stubs for less-used methods */
static void NativeBN_BN_bin2bn(JNIEnv* env, jclass cls, jbyteArray s, jint len, jboolean neg, jlong ret) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)ret;
    if (!bn || !s) return;
    jbyte* data = (*env)->GetByteArrayElements(env, s, NULL);
    long v = 0;
    for (int i = 0; i < len && i < 8; i++) v = (v << 8) | (data[i] & 0xFF);
    (*env)->ReleaseByteArrayElements(env, s, data, JNI_ABORT);
    bn->val = v; bn->neg = neg ? 1 : 0;
}
static jbyteArray NativeBN_BN_bn2bin(JNIEnv* env, jclass cls, jlong a) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    if (!bn) { jbyteArray r = (*env)->NewByteArray(env, 1); return r; }
    long v = bn->val;
    int bytes = 0; long tmp = v;
    do { bytes++; tmp >>= 8; } while (tmp > 0);
    jbyteArray r = (*env)->NewByteArray(env, bytes);
    jbyte buf[8];
    for (int i = bytes - 1; i >= 0; i--) { buf[i] = (jbyte)(v & 0xFF); v >>= 8; }
    (*env)->SetByteArrayRegion(env, r, 0, bytes, buf);
    return r;
}
static void NativeBN_litEndInts2bn(JNIEnv* env, jclass cls, jintArray ints, jint len, jboolean neg, jlong ret) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)ret;
    if (!bn || !ints || len == 0) return;
    jint* data = (*env)->GetIntArrayElements(env, ints, NULL);
    long v = 0;
    for (int i = len - 1; i >= 0 && i < 2; i--) v = (v << 32) | ((long)data[i] & 0xFFFFFFFFL);
    (*env)->ReleaseIntArrayElements(env, ints, data, JNI_ABORT);
    bn->val = v; bn->neg = neg ? 1 : 0;
}
static jintArray NativeBN_bn2litEndInts(JNIEnv* env, jclass cls, jlong a) {
    SimpleBN* bn = (SimpleBN*)(uintptr_t)a;
    long v = bn ? bn->val : 0;
    int count = v > 0xFFFFFFFFL ? 2 : 1;
    jintArray r = (*env)->NewIntArray(env, count);
    jint buf[2] = { (jint)(v & 0xFFFFFFFFL), (jint)((v >> 32) & 0xFFFFFFFFL) };
    (*env)->SetIntArrayRegion(env, r, 0, count, buf);
    return r;
}
static void NativeBN_twosComp2bn(JNIEnv* env, jclass cls, jbyteArray s, jint len, jlong ret) {
    NativeBN_BN_bin2bn(env, cls, s, len, 0, ret);
}

/* NativeBN arithmetic operations */
static void NativeBN_BN_sub(JNIEnv* env, jclass cls, jlong r, jlong a, jlong b) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)r;
    SimpleBN* ba = (SimpleBN*)(uintptr_t)a;
    SimpleBN* bb = (SimpleBN*)(uintptr_t)b;
    if (!br || !ba || !bb) return;
    long va = ba->neg ? -ba->val : ba->val;
    long vb = bb->neg ? -bb->val : bb->val;
    long vr = va - vb;
    br->neg = vr < 0 ? 1 : 0; br->val = vr < 0 ? -vr : vr;
}
static void NativeBN_BN_mul(JNIEnv* env, jclass cls, jlong r, jlong a, jlong b) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)r;
    SimpleBN* ba = (SimpleBN*)(uintptr_t)a;
    SimpleBN* bb = (SimpleBN*)(uintptr_t)b;
    if (!br || !ba || !bb) return;
    br->val = ba->val * bb->val;
    br->neg = (ba->neg != bb->neg) ? 1 : 0;
    if (br->val == 0) br->neg = 0;
}
static void NativeBN_BN_div(JNIEnv* env, jclass cls, jlong dv, jlong rem, jlong m, jlong d) {
    SimpleBN* bdv = (SimpleBN*)(uintptr_t)dv;
    SimpleBN* brem = (SimpleBN*)(uintptr_t)rem;
    SimpleBN* bm = (SimpleBN*)(uintptr_t)m;
    SimpleBN* bd = (SimpleBN*)(uintptr_t)d;
    if (!bm || !bd || bd->val == 0) return;
    long vm = bm->neg ? -bm->val : bm->val;
    long vd = bd->neg ? -bd->val : bd->val;
    if (bdv) { long q = vm / vd; bdv->neg = q < 0 ? 1 : 0; bdv->val = q < 0 ? -q : q; }
    if (brem) { long r2 = vm % vd; brem->neg = r2 < 0 ? 1 : 0; brem->val = r2 < 0 ? -r2 : r2; }
}
static void NativeBN_BN_exp(JNIEnv* env, jclass cls, jlong r, jlong a, jlong p) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)r;
    SimpleBN* ba = (SimpleBN*)(uintptr_t)a;
    SimpleBN* bp = (SimpleBN*)(uintptr_t)p;
    if (!br || !ba || !bp) return;
    long base = ba->val, exp = bp->val, result = 1;
    for (long i = 0; i < exp && i < 62; i++) result *= base;
    br->val = result; br->neg = (ba->neg && (exp & 1)) ? 1 : 0;
}
static void NativeBN_BN_gcd(JNIEnv* env, jclass cls, jlong r, jlong a, jlong b) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)r;
    SimpleBN* ba = (SimpleBN*)(uintptr_t)a;
    SimpleBN* bb = (SimpleBN*)(uintptr_t)b;
    if (!br || !ba || !bb) return;
    long x = ba->val, y = bb->val;
    while (y != 0) { long t = y; y = x % y; x = t; }
    br->val = x; br->neg = 0;
}
static void NativeBN_BN_nnmod(JNIEnv* env, jclass cls, jlong r, jlong a, jlong m) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)r;
    SimpleBN* ba = (SimpleBN*)(uintptr_t)a;
    SimpleBN* bm = (SimpleBN*)(uintptr_t)m;
    if (!br || !ba || !bm || bm->val == 0) return;
    long va = ba->neg ? -ba->val : ba->val;
    long vr = va % bm->val;
    if (vr < 0) vr += bm->val;
    br->val = vr; br->neg = 0;
}
static void NativeBN_BN_mod_exp(JNIEnv* env, jclass cls, jlong r, jlong a, jlong p, jlong m) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)r;
    SimpleBN* ba = (SimpleBN*)(uintptr_t)a;
    SimpleBN* bp = (SimpleBN*)(uintptr_t)p;
    SimpleBN* bm = (SimpleBN*)(uintptr_t)m;
    if (!br || !ba || !bp || !bm || bm->val == 0) return;
    long base = ba->val % bm->val, exp = bp->val, mod = bm->val, result = 1;
    base %= mod;
    while (exp > 0) {
        if (exp & 1) result = (result * base) % mod;
        exp >>= 1; base = (base * base) % mod;
    }
    br->val = result; br->neg = 0;
}
static void NativeBN_BN_mod_inverse(JNIEnv* env, jclass cls, jlong ret, jlong a, jlong n) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)ret;
    if (br) { br->val = 1; br->neg = 0; } /* stub */
}
static void NativeBN_BN_generate_prime_ex(JNIEnv* env, jclass cls, jlong ret, jint bits,
        jboolean safe, jlong add, jlong rem) {
    SimpleBN* br = (SimpleBN*)(uintptr_t)ret;
    if (br) { br->val = (1L << (bits > 62 ? 62 : bits)) - 1; br->neg = 0; }
}
static jboolean NativeBN_BN_primality_test(JNIEnv* env, jclass cls, jlong candidate,
        jint checks, jboolean trial) {
    return JNI_FALSE; /* not prime — safe default */
}

/* ==================== Android 15 libcore.math.NativeBN ==================== */
/*
 * Android 15 moved BigInteger's native surface to libcore.math.NativeBN and
 * reduced it to these seven methods. Keep the older java.math.NativeBN stubs
 * above for legacy boot class paths, but back the Android 15 ABI with the
 * platform OpenSSL implementation so certificate RSA operations are not
 * truncated to the legacy stub's machine-word representation.
 *
 * Resolve OpenSSL dynamically: libart is shared across bridge configurations,
 * while OpenHarmony supplies libcrypto_openssl.z.so at the platform boundary.
 */
typedef struct oh_bignum_st OhBignum;
typedef struct oh_bn_ctx_st OhBnCtx;

typedef struct {
    void* handle;
    OhBignum* (*bn_new)(void);
    void (*bn_free)(OhBignum*);
    OhBignum* (*bin2bn)(const unsigned char*, int, OhBignum*);
    int (*bn2bin)(const OhBignum*, unsigned char*);
    int (*num_bits)(const OhBignum*);
    void (*set_negative)(OhBignum*, int);
    OhBnCtx* (*ctx_new)(void);
    void (*ctx_free)(OhBnCtx*);
    int (*mul)(OhBignum*, const OhBignum*, const OhBignum*, OhBnCtx*);
    int (*div)(OhBignum*, OhBignum*, const OhBignum*, const OhBignum*, OhBnCtx*);
    int (*mod_exp)(OhBignum*, const OhBignum*, const OhBignum*, const OhBignum*, OhBnCtx*);
    const char* error;
} OhNativeBnApi;

static OhNativeBnApi gOhNativeBn;
static pthread_once_t gOhNativeBnOnce = PTHREAD_ONCE_INIT;

static void initOhNativeBn(void) {
    gOhNativeBn.handle = dlopen("libcrypto_openssl.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!gOhNativeBn.handle) {
        gOhNativeBn.error = dlerror();
        return;
    }

#define LOAD_OH_BN(field, symbol)                                                \
    do {                                                                         \
        *(void**)(&gOhNativeBn.field) = dlsym(gOhNativeBn.handle, symbol);       \
        if (!gOhNativeBn.field) {                                                 \
            gOhNativeBn.error = "libcrypto_openssl.z.so is missing " symbol;    \
            return;                                                              \
        }                                                                        \
    } while (0)

    LOAD_OH_BN(bn_new, "BN_new");
    LOAD_OH_BN(bn_free, "BN_free");
    LOAD_OH_BN(bin2bn, "BN_bin2bn");
    LOAD_OH_BN(bn2bin, "BN_bn2bin");
    LOAD_OH_BN(num_bits, "BN_num_bits");
    LOAD_OH_BN(set_negative, "BN_set_negative");
    LOAD_OH_BN(ctx_new, "BN_CTX_new");
    LOAD_OH_BN(ctx_free, "BN_CTX_free");
    LOAD_OH_BN(mul, "BN_mul");
    LOAD_OH_BN(div, "BN_div");
    LOAD_OH_BN(mod_exp, "BN_mod_exp");
#undef LOAD_OH_BN
}

static void throwNativeBnException(JNIEnv* env, const char* className, const char* message) {
    jclass cls = (*env)->FindClass(env, className);
    if (cls) {
        (*env)->ThrowNew(env, cls, message);
        (*env)->DeleteLocalRef(env, cls);
    }
}

static jboolean ensureOhNativeBn(JNIEnv* env) {
    pthread_once(&gOhNativeBnOnce, initOhNativeBn);
    if (!gOhNativeBn.error && gOhNativeBn.mod_exp) return JNI_TRUE;
    throwNativeBnException(env, "java/lang/UnsatisfiedLinkError",
            gOhNativeBn.error ? gOhNativeBn.error : "OpenHarmony OpenSSL NativeBN is unavailable");
    return JNI_FALSE;
}

static OhBignum* toOhBignum(jlong address) {
    return (OhBignum*)(uintptr_t)address;
}

static jboolean requireOhBignum(JNIEnv* env, jlong address) {
    if (address != 0) return JNI_TRUE;
    throwNativeBnException(env, "java/lang/NullPointerException", "null BIGNUM pointer");
    return JNI_FALSE;
}

static void throwOhBnOperationFailed(JNIEnv* env) {
    throwNativeBnException(env, "java/lang/ArithmeticException", "BIGNUM operation failed");
}

static jlong LibcoreNativeBN_BN_new(JNIEnv* env, jclass cls) {
    (void)cls;
    if (!ensureOhNativeBn(env)) return 0;
    OhBignum* result = gOhNativeBn.bn_new();
    if (!result) {
        throwNativeBnException(env, "java/lang/OutOfMemoryError", "BN_new failed");
        return 0;
    }
    return (jlong)(uintptr_t)result;
}

static void LibcoreNativeBN_BN_free(JNIEnv* env, jclass cls, jlong a) {
    (void)cls;
    if (!ensureOhNativeBn(env)) return;
    gOhNativeBn.bn_free(toOhBignum(a));
}

static void LibcoreNativeBN_litEndInts2bn(JNIEnv* env, jclass cls, jintArray ints,
        jint len, jboolean neg, jlong retAddress) {
    (void)cls;
    if (!ensureOhNativeBn(env) || !requireOhBignum(env, retAddress)) return;
    if (!ints) {
        throwNativeBnException(env, "java/lang/NullPointerException", "ints == null");
        return;
    }

    jsize arrayLen = (*env)->GetArrayLength(env, ints);
    if (len < 0 || len > arrayLen || len > INT_MAX / (jint)sizeof(jint)) {
        throwNativeBnException(env, "java/lang/IllegalArgumentException", "invalid NativeBN word count");
        return;
    }

    size_t numBytes = (size_t)len * sizeof(jint);
    unsigned char zero = 0;
    unsigned char* bigEndian = numBytes ? (unsigned char*)malloc(numBytes) : &zero;
    jint* words = len ? (jint*)malloc((size_t)len * sizeof(jint)) : NULL;
    if ((numBytes && !bigEndian) || (len && !words)) {
        if (numBytes && bigEndian) free(bigEndian);
        free(words);
        throwNativeBnException(env, "java/lang/OutOfMemoryError", "NativeBN conversion allocation failed");
        return;
    }

    if (len) {
        (*env)->GetIntArrayRegion(env, ints, 0, len, words);
        if ((*env)->ExceptionCheck(env)) {
            free(bigEndian);
            free(words);
            return;
        }
        for (jint wordIndex = 0; wordIndex < len; ++wordIndex) {
            uint32_t word = (uint32_t)words[wordIndex];
            for (unsigned int byteIndex = 0; byteIndex < sizeof(jint); ++byteIndex) {
                size_t littleByteIndex = (size_t)wordIndex * sizeof(jint) + byteIndex;
                bigEndian[numBytes - 1 - littleByteIndex] =
                        (unsigned char)(word >> (8u * byteIndex));
            }
        }
    }

    OhBignum* result = gOhNativeBn.bin2bn(bigEndian, (int)numBytes, toOhBignum(retAddress));
    if (numBytes) free(bigEndian);
    free(words);
    if (!result) {
        throwOhBnOperationFailed(env);
        return;
    }
    gOhNativeBn.set_negative(result, neg ? 1 : 0);
}

static jintArray LibcoreNativeBN_bn2litEndInts(JNIEnv* env, jclass cls, jlong aAddress) {
    (void)cls;
    if (!ensureOhNativeBn(env) || !requireOhBignum(env, aAddress)) return NULL;

    OhBignum* a = toOhBignum(aAddress);
    int bits = gOhNativeBn.num_bits(a);
    if (bits < 0) {
        throwOhBnOperationFailed(env);
        return NULL;
    }
    size_t numBytes = ((size_t)bits + 7u) / 8u;
    size_t intLen = (numBytes + sizeof(jint) - 1u) / sizeof(jint);
    if (intLen > (size_t)INT_MAX) {
        throwNativeBnException(env, "java/lang/OutOfMemoryError", "NativeBN result is too large");
        return NULL;
    }

    jintArray result = (*env)->NewIntArray(env, (jsize)intLen);
    if (!result || intLen == 0) return result;

    unsigned char* bigEndian = (unsigned char*)malloc(numBytes);
    jint* words = (jint*)calloc(intLen, sizeof(jint));
    if (!bigEndian || !words) {
        free(bigEndian);
        free(words);
        throwNativeBnException(env, "java/lang/OutOfMemoryError", "NativeBN conversion allocation failed");
        return NULL;
    }
    if (gOhNativeBn.bn2bin(a, bigEndian) != (int)numBytes) {
        free(bigEndian);
        free(words);
        throwOhBnOperationFailed(env);
        return NULL;
    }

    for (size_t littleByteIndex = 0; littleByteIndex < numBytes; ++littleByteIndex) {
        unsigned char value = bigEndian[numBytes - 1u - littleByteIndex];
        size_t wordIndex = littleByteIndex / sizeof(jint);
        unsigned int shift = (unsigned int)(8u * (littleByteIndex % sizeof(jint)));
        words[wordIndex] = (jint)((uint32_t)words[wordIndex] | ((uint32_t)value << shift));
    }
    (*env)->SetIntArrayRegion(env, result, 0, (jsize)intLen, words);
    free(bigEndian);
    free(words);
    return result;
}

static OhBnCtx* newOhBnContext(JNIEnv* env) {
    OhBnCtx* ctx = gOhNativeBn.ctx_new();
    if (!ctx) throwNativeBnException(env, "java/lang/OutOfMemoryError", "BN_CTX_new failed");
    return ctx;
}

static void LibcoreNativeBN_BN_mul(JNIEnv* env, jclass cls, jlong r, jlong a, jlong b) {
    (void)cls;
    if (!ensureOhNativeBn(env) || !requireOhBignum(env, r) ||
            !requireOhBignum(env, a) || !requireOhBignum(env, b)) return;
    OhBnCtx* ctx = newOhBnContext(env);
    if (!ctx) return;
    int ok = gOhNativeBn.mul(toOhBignum(r), toOhBignum(a), toOhBignum(b), ctx);
    gOhNativeBn.ctx_free(ctx);
    if (!ok) throwOhBnOperationFailed(env);
}

static void LibcoreNativeBN_BN_div(JNIEnv* env, jclass cls, jlong quotient, jlong remainder,
        jlong numerator, jlong divisor) {
    (void)cls;
    if (!ensureOhNativeBn(env) || !requireOhBignum(env, numerator) ||
            !requireOhBignum(env, divisor)) return;
    OhBnCtx* ctx = newOhBnContext(env);
    if (!ctx) return;
    int ok = gOhNativeBn.div(toOhBignum(quotient), toOhBignum(remainder),
            toOhBignum(numerator), toOhBignum(divisor), ctx);
    gOhNativeBn.ctx_free(ctx);
    if (!ok) throwOhBnOperationFailed(env);
}

static void LibcoreNativeBN_BN_mod_exp(JNIEnv* env, jclass cls, jlong r, jlong a,
        jlong exponent, jlong modulus) {
    (void)cls;
    if (!ensureOhNativeBn(env) || !requireOhBignum(env, r) ||
            !requireOhBignum(env, a) || !requireOhBignum(env, exponent) ||
            !requireOhBignum(env, modulus)) return;
    OhBnCtx* ctx = newOhBnContext(env);
    if (!ctx) return;
    int ok = gOhNativeBn.mod_exp(toOhBignum(r), toOhBignum(a), toOhBignum(exponent),
            toOhBignum(modulus), ctx);
    gOhNativeBn.ctx_free(ctx);
    if (!ok) throwOhBnOperationFailed(env);
}

/* ==================== ICU / LocaleData stubs ==================== */

/* Helper to set a String field on LocaleData */
static void ld_setString(JNIEnv* env, jobject ld, jclass cls, const char* name, const char* val) {
    jfieldID f = (*env)->GetFieldID(env, cls, name, "Ljava/lang/String;");
    if (f) (*env)->SetObjectField(env, ld, f, (*env)->NewStringUTF(env, val));
    else (*env)->ExceptionClear(env);
}
/* Helper to set a char field */
static void ld_setChar(JNIEnv* env, jobject ld, jclass cls, const char* name, jchar val) {
    jfieldID f = (*env)->GetFieldID(env, cls, name, "C");
    if (f) (*env)->SetCharField(env, ld, f, val);
    else (*env)->ExceptionClear(env);
}
/* Helper to set an Integer field */
static void ld_setInteger(JNIEnv* env, jobject ld, jclass cls, const char* name, int val) {
    jfieldID f = (*env)->GetFieldID(env, cls, name, "Ljava/lang/Integer;");
    if (!f) { (*env)->ExceptionClear(env); return; }
    jclass intCls = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID valueOf = (*env)->GetStaticMethodID(env, intCls, "valueOf", "(I)Ljava/lang/Integer;");
    jobject obj = (*env)->CallStaticObjectMethod(env, intCls, valueOf, val);
    (*env)->SetObjectField(env, ld, f, obj);
}
/* Helper to set a String[] field */
static void ld_setStringArray(JNIEnv* env, jobject ld, jclass cls, const char* name,
                              const char** vals, int count) {
    jfieldID f = (*env)->GetFieldID(env, cls, name, "[Ljava/lang/String;");
    if (!f) { (*env)->ExceptionClear(env); return; }
    jclass strCls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, count, strCls, NULL);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return;
    }
    (void)vals;
    /* The standalone runtime can carry duplicate java.lang.String class objects across
       native/JNI boundaries. Repeated SetObjectArrayElement calls for LocaleData calendar
       name arrays therefore trip class-identity ArrayStoreExceptions and can destabilize
       the native thread. The McD startup path needs the scalar number/currency fields below,
       so keep the String[] field shape here and leave the calendar-name entries unset. */
    (*env)->SetObjectField(env, ld, f, arr);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
}

/* getBestDateTimePatternNative(skeleton, languageTag) → pattern string */
static jstring ICU_getBestDateTimePatternNative(JNIEnv* env, jclass cls,
        jstring skeleton, jstring languageTag) {
    return skeleton ? skeleton : (*env)->NewStringUTF(env, "yyyy-MM-dd HH:mm:ss");
}
static jstring ICU_getCurrencyCode(JNIEnv* env, jclass cls, jstring countryCode) {
    return (*env)->NewStringUTF(env, "USD");
}
static jstring ICU_getISO3Country(JNIEnv* env, jclass cls, jstring languageTag) {
    return (*env)->NewStringUTF(env, "USA");
}
static jstring ICU_getISO3Language(JNIEnv* env, jclass cls, jstring languageTag) {
    return (*env)->NewStringUTF(env, "eng");
}
static jstring ICU_getScript(JNIEnv* env, jclass cls, jstring locale) {
    return (*env)->NewStringUTF(env, "");
}
static jobjectArray ICU_getISOLanguagesNative(JNIEnv* env, jclass cls) {
    jclass strCls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, 1, strCls, NULL);
    (*env)->SetObjectArrayElement(env, arr, 0, (*env)->NewStringUTF(env, "en"));
    return arr;
}
static jobjectArray ICU_getISOCountriesNative(JNIEnv* env, jclass cls) {
    jclass strCls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, 1, strCls, NULL);
    (*env)->SetObjectArrayElement(env, arr, 0, (*env)->NewStringUTF(env, "US"));
    return arr;
}
static jobjectArray ICU_getAvailableLocalesNative(JNIEnv* env, jclass cls) {
    jclass strCls = (*env)->FindClass(env, "java/lang/String");
    const char* locales[] = {"en_US", "en", ""};
    jobjectArray arr = (*env)->NewObjectArray(env, 3, strCls, NULL);
    for (int i = 0; i < 3; i++)
        (*env)->SetObjectArrayElement(env, arr, i, (*env)->NewStringUTF(env, locales[i]));
    return arr;
}
static void ICU_setDefaultLocale(JNIEnv* env, jclass cls, jstring languageTag) { /* no-op */ }
static jstring ICU_getDefaultLocale(JNIEnv* env, jclass cls) {
    return (*env)->NewStringUTF(env, "en_US");
}

static jboolean ICU_initLocaleDataNative(JNIEnv* env, jclass thiz,
        jstring languageTag, jobject localeData) {
    jclass cls = (*env)->GetObjectClass(env, localeData);
    if (!cls) return JNI_FALSE;

    /* Calendar */
    ld_setInteger(env, localeData, cls, "firstDayOfWeek", 1);         /* Sunday */
    ld_setInteger(env, localeData, cls, "minimalDaysInFirstWeek", 1);

    /* AM/PM */
    const char* amPm[] = {"AM", "PM"};
    ld_setStringArray(env, localeData, cls, "amPm", amPm, 2);

    /* Month names (13 entries — Java Calendar months are 0-based, 12=Undecimber) */
    const char* longMonths[] = {"January","February","March","April","May","June",
        "July","August","September","October","November","December",""};
    const char* shortMonths[] = {"Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec",""};
    const char* tinyMonths[] = {"J","F","M","A","M","J","J","A","S","O","N","D",""};
    ld_setStringArray(env, localeData, cls, "longMonthNames", longMonths, 13);
    ld_setStringArray(env, localeData, cls, "shortMonthNames", shortMonths, 13);
    ld_setStringArray(env, localeData, cls, "tinyMonthNames", tinyMonths, 13);
    ld_setStringArray(env, localeData, cls, "longStandAloneMonthNames", longMonths, 13);
    ld_setStringArray(env, localeData, cls, "shortStandAloneMonthNames", shortMonths, 13);
    ld_setStringArray(env, localeData, cls, "tinyStandAloneMonthNames", tinyMonths, 13);

    /* Weekday names (8 entries — Java Calendar days are 1-based, index 0 is empty) */
    const char* longDays[] = {"","Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    const char* shortDays[] = {"","Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char* tinyDays[] = {"","S","M","T","W","T","F","S"};
    ld_setStringArray(env, localeData, cls, "longWeekdayNames", longDays, 8);
    ld_setStringArray(env, localeData, cls, "shortWeekdayNames", shortDays, 8);
    ld_setStringArray(env, localeData, cls, "tinyWeekdayNames", tinyDays, 8);
    ld_setStringArray(env, localeData, cls, "longStandAloneWeekdayNames", longDays, 8);
    ld_setStringArray(env, localeData, cls, "shortStandAloneWeekdayNames", shortDays, 8);
    ld_setStringArray(env, localeData, cls, "tinyStandAloneWeekdayNames", tinyDays, 8);

    /* Eras */
    const char* eras[] = {"BC", "AD"};
    ld_setStringArray(env, localeData, cls, "eras", eras, 2);

    /* Relative days */
    ld_setString(env, localeData, cls, "yesterday", "Yesterday");
    ld_setString(env, localeData, cls, "today", "Today");
    ld_setString(env, localeData, cls, "tomorrow", "Tomorrow");

    /* Time/date formats */
    ld_setString(env, localeData, cls, "fullTimeFormat", "h:mm:ss a zzzz");
    ld_setString(env, localeData, cls, "longTimeFormat", "h:mm:ss a z");
    ld_setString(env, localeData, cls, "mediumTimeFormat", "h:mm:ss a");
    ld_setString(env, localeData, cls, "shortTimeFormat", "h:mm a");
    ld_setString(env, localeData, cls, "fullDateFormat", "EEEE, MMMM d, y");
    ld_setString(env, localeData, cls, "longDateFormat", "MMMM d, y");
    ld_setString(env, localeData, cls, "mediumDateFormat", "MMM d, y");
    ld_setString(env, localeData, cls, "shortDateFormat", "M/d/yy");

    /* 12/24hr time variants */
    ld_setString(env, localeData, cls, "timeFormat_hm", "h:mm a");
    ld_setString(env, localeData, cls, "timeFormat_Hm", "HH:mm");
    ld_setString(env, localeData, cls, "timeFormat_hms", "h:mm:ss a");
    ld_setString(env, localeData, cls, "timeFormat_Hms", "HH:mm:ss");
    ld_setString(env, localeData, cls, "narrowAm", "a");
    ld_setString(env, localeData, cls, "narrowPm", "p");

    /* Number formatting */
    ld_setChar(env, localeData, cls, "zeroDigit", '0');
    ld_setChar(env, localeData, cls, "decimalSeparator", '.');
    ld_setChar(env, localeData, cls, "groupingSeparator", ',');
    ld_setChar(env, localeData, cls, "patternSeparator", ';');
    ld_setString(env, localeData, cls, "percent", "%");
    ld_setString(env, localeData, cls, "perMill", "\u2030");
    ld_setChar(env, localeData, cls, "monetarySeparator", '.');
    ld_setString(env, localeData, cls, "minusSign", "-");
    ld_setString(env, localeData, cls, "exponentSeparator", "E");
    ld_setString(env, localeData, cls, "infinity", "\u221E");
    ld_setString(env, localeData, cls, "NaN", "NaN");

    /* Currency */
    ld_setString(env, localeData, cls, "currencySymbol", "$");
    ld_setString(env, localeData, cls, "internationalCurrencySymbol", "USD");

    /* Number patterns */
    ld_setString(env, localeData, cls, "numberPattern", "#,##0.###");
    ld_setString(env, localeData, cls, "integerPattern", "#,##0");
    ld_setString(env, localeData, cls, "currencyPattern", "\u00A4#,##0.00");
    ld_setString(env, localeData, cls, "percentPattern", "#,##0%");

    return JNI_TRUE;
}

/* AsynchronousCloseMonitor stubs */
static void ACM_signalBlockedThreads(JNIEnv* env, jclass cls, jobject fd) { /* no-op */ }

/* Thread.nicenessForPriority (Android 14+) — maps Java priority 1-10 to Linux niceness */
static jint Thread_nicenessForPriority(JNIEnv* env, jclass cls, jint priority) {
    /* Standard Android mapping: MIN_PRIORITY(1)→19, NORM(5)→0, MAX(10)→-8 */
    static const int kNiceValues[] = {19, 16, 13, 10, 0, -2, -4, -5, -6, -8};
    if (priority < 1) priority = 1;
    if (priority > 10) priority = 10;
    return kNiceValues[priority - 1];
}

/* java.util.zip CRC/Adler support. Avoid GetPrimitiveArrayCritical here: the
 * standalone runtime can move through early multidex code before critical-array
 * pinning is fully safe. */
static jint CRC32_update(JNIEnv* env, jclass cls, jint crc, jint b) {
    unsigned char one = (unsigned char)b;
    return (jint)crc32((uLong)crc, &one, 1);
}

static jint CRC32_updateBytes(JNIEnv* env, jclass cls, jint crc,
                              jbyteArray array, jint off, jint len) {
    if (!array || len <= 0) return crc;
    jsize array_len = (*env)->GetArrayLength(env, array);
    if (off < 0 || len < 0 || off > array_len || len > array_len - off) return crc;
    jbyte* bytes = (*env)->GetByteArrayElements(env, array, NULL);
    if (!bytes) return crc;
    jint out = (jint)crc32((uLong)crc, (const Bytef*)(bytes + off), (uInt)len);
    (*env)->ReleaseByteArrayElements(env, array, bytes, JNI_ABORT);
    return out;
}

static jint CRC32_updateByteBuffer(JNIEnv* env, jclass cls, jint crc,
                                   jlong address, jint off, jint len) {
    if (address == 0 || len <= 0) return crc;
    const Bytef* bytes = (const Bytef*)((uintptr_t)address + (uintptr_t)off);
    return (jint)crc32((uLong)crc, bytes, (uInt)len);
}

static jint Adler32_update(JNIEnv* env, jclass cls, jint adler, jint b) {
    unsigned char one = (unsigned char)b;
    return (jint)adler32((uLong)adler, &one, 1);
}

static jint Adler32_updateBytes(JNIEnv* env, jclass cls, jint adler,
                                jbyteArray array, jint off, jint len) {
    if (!array || len <= 0) return adler;
    jsize array_len = (*env)->GetArrayLength(env, array);
    if (off < 0 || len < 0 || off > array_len || len > array_len - off) return adler;
    jbyte* bytes = (*env)->GetByteArrayElements(env, array, NULL);
    if (!bytes) return adler;
    jint out = (jint)adler32((uLong)adler, (const Bytef*)(bytes + off), (uInt)len);
    (*env)->ReleaseByteArrayElements(env, array, bytes, JNI_ABORT);
    return out;
}

static jint Adler32_updateByteBuffer(JNIEnv* env, jclass cls, jint adler,
                                     jlong address, jint off, jint len) {
    if (address == 0 || len <= 0) return adler;
    const Bytef* bytes = (const Bytef*)((uintptr_t)address + (uintptr_t)off);
    return (jint)adler32((uLong)adler, bytes, (uInt)len);
}

/* java.util.zip Deflater/Inflater support for multidex ZIP extraction. Android
 * core-oj packs native progress into a long: low 31 bits are input consumed,
 * next 31 bits are output produced, bit 62 is finished, bit 63 is the
 * class-specific "pending parameter/dictionary" flag. */
static jlong zipPackResult(jint consumed, jint produced, int bit62, int bit63) {
    uint64_t packed = ((uint64_t)consumed & 0x7fffffffULL) |
                      (((uint64_t)produced & 0x7fffffffULL) << 31);
    if (bit62) packed |= (1ULL << 62);
    if (bit63) packed |= (1ULL << 63);
    return (jlong)packed;
}

static z_stream* zipStreamFromAddress(jlong addr) {
    return (z_stream*)(uintptr_t)addr;
}

static const char* zipMessage(z_stream* zs, const char* fallback) {
    return (zs && zs->msg) ? zs->msg : fallback;
}

static void setIntFieldIfPresent(JNIEnv* env, jobject obj, const char* name, jint value) {
    if (!obj) return;
    jclass cls = (*env)->GetObjectClass(env, obj);
    if (!cls) return;
    jfieldID field = (*env)->GetFieldID(env, cls, name, "I");
    if (!field) {
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->SetIntField(env, obj, field, value);
}

static jlong Deflater_init(JNIEnv* env, jclass cls, jint level, jint strategy, jboolean nowrap) {
    z_stream* zs = (z_stream*)calloc(1, sizeof(z_stream));
    if (!zs) {
        throwOutOfMemoryError(env, "zlib deflate stream allocation failed");
        return 0;
    }

    int windowBits = nowrap ? -MAX_WBITS : MAX_WBITS;
    int rc = deflateInit2(zs, level, Z_DEFLATED, windowBits, DEF_MEM_LEVEL, strategy);
    if (rc != Z_OK) {
        const char* msg = zipMessage(zs, "deflateInit2 failed");
        deflateEnd(zs);
        free(zs);
        throwZipException(env, msg);
        return 0;
    }
    return (jlong)(uintptr_t)zs;
}

static void Deflater_setDictionary(JNIEnv* env, jclass cls,
                                   jlong addr, jbyteArray array, jint off, jint len) {
    z_stream* zs = zipStreamFromAddress(addr);
    if (!zs || !array || len <= 0) return;
    jsize arrayLen = (*env)->GetArrayLength(env, array);
    if (off < 0 || len < 0 || off > arrayLen || len > arrayLen - off) return;
    jbyte* bytes = (*env)->GetByteArrayElements(env, array, NULL);
    if (!bytes) return;
    int rc = deflateSetDictionary(zs, (const Bytef*)(bytes + off), (uInt)len);
    (*env)->ReleaseByteArrayElements(env, array, bytes, JNI_ABORT);
    if (rc != Z_OK) throwZipException(env, zipMessage(zs, "deflateSetDictionary failed"));
}

static void Deflater_setDictionaryBuffer(JNIEnv* env, jclass cls,
                                         jlong addr, jlong buffer, jint len) {
    z_stream* zs = zipStreamFromAddress(addr);
    if (!zs || buffer == 0 || len <= 0) return;
    int rc = deflateSetDictionary(zs, (const Bytef*)(uintptr_t)buffer, (uInt)len);
    if (rc != Z_OK) throwZipException(env, zipMessage(zs, "deflateSetDictionary failed"));
}

static jlong Deflater_deflateCommon(JNIEnv* env, z_stream* zs,
                                    Bytef* in, jint inLen,
                                    Bytef* out, jint outLen,
                                    jint flush, jint params) {
    if (!zs) {
        throwZipException(env, "deflater stream is closed");
        return 0;
    }
    if (inLen < 0) inLen = 0;
    if (outLen < 0) outLen = 0;

    uLong oldIn = zs->total_in;
    uLong oldOut = zs->total_out;
    zs->next_in = inLen > 0 ? in : Z_NULL;
    zs->avail_in = (uInt)inLen;
    zs->next_out = outLen > 0 ? out : Z_NULL;
    zs->avail_out = (uInt)outLen;

    int paramsPending = 0;
    if (params != 0) {
        int strategy = (params >> 1) & 0x3;
        int level = params >> 3;
        int rc = deflateParams(zs, level, strategy);
        if (rc == Z_BUF_ERROR) {
            paramsPending = 1;
        } else if (rc != Z_OK) {
            throwZipException(env, zipMessage(zs, "deflateParams failed"));
            return 0;
        }
    }

    int rc = deflate(zs, flush);
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
        throwZipException(env, zipMessage(zs, "deflate failed"));
        return 0;
    }

    jint consumed = (jint)(zs->total_in - oldIn);
    jint produced = (jint)(zs->total_out - oldOut);
    return zipPackResult(consumed, produced, rc == Z_STREAM_END, paramsPending);
}

static jlong Deflater_deflateBytesBytes(JNIEnv* env, jobject thiz, jlong addr,
                                        jbyteArray inArray, jint inOff, jint inLen,
                                        jbyteArray outArray, jint outOff, jint outLen,
                                        jint flush, jint params) {
    jbyte* inBytes = NULL;
    jbyte* outBytes = NULL;
    if (inArray && inLen > 0) {
        inBytes = (*env)->GetByteArrayElements(env, inArray, NULL);
        if (!inBytes) return 0;
    }
    if (outArray && outLen > 0) {
        outBytes = (*env)->GetByteArrayElements(env, outArray, NULL);
        if (!outBytes) {
            if (inBytes) (*env)->ReleaseByteArrayElements(env, inArray, inBytes, JNI_ABORT);
            return 0;
        }
    }
    jlong result = Deflater_deflateCommon(env, zipStreamFromAddress(addr),
                                          inBytes ? (Bytef*)(inBytes + inOff) : NULL, inLen,
                                          outBytes ? (Bytef*)(outBytes + outOff) : NULL, outLen,
                                          flush, params);
    if (inBytes) (*env)->ReleaseByteArrayElements(env, inArray, inBytes, JNI_ABORT);
    if (outBytes) (*env)->ReleaseByteArrayElements(env, outArray, outBytes, 0);
    return result;
}

static jlong Deflater_deflateBytesBuffer(JNIEnv* env, jobject thiz, jlong addr,
                                         jbyteArray inArray, jint inOff, jint inLen,
                                         jlong outAddress, jint outLen,
                                         jint flush, jint params) {
    jbyte* inBytes = NULL;
    if (inArray && inLen > 0) {
        inBytes = (*env)->GetByteArrayElements(env, inArray, NULL);
        if (!inBytes) return 0;
    }
    jlong result = Deflater_deflateCommon(env, zipStreamFromAddress(addr),
                                          inBytes ? (Bytef*)(inBytes + inOff) : NULL, inLen,
                                          outAddress ? (Bytef*)(uintptr_t)outAddress : NULL, outLen,
                                          flush, params);
    if (inBytes) (*env)->ReleaseByteArrayElements(env, inArray, inBytes, JNI_ABORT);
    return result;
}

static jlong Deflater_deflateBufferBytes(JNIEnv* env, jobject thiz, jlong addr,
                                         jlong inAddress, jint inLen,
                                         jbyteArray outArray, jint outOff, jint outLen,
                                         jint flush, jint params) {
    jbyte* outBytes = NULL;
    if (outArray && outLen > 0) {
        outBytes = (*env)->GetByteArrayElements(env, outArray, NULL);
        if (!outBytes) return 0;
    }
    jlong result = Deflater_deflateCommon(env, zipStreamFromAddress(addr),
                                          inAddress ? (Bytef*)(uintptr_t)inAddress : NULL, inLen,
                                          outBytes ? (Bytef*)(outBytes + outOff) : NULL, outLen,
                                          flush, params);
    if (outBytes) (*env)->ReleaseByteArrayElements(env, outArray, outBytes, 0);
    return result;
}

static jlong Deflater_deflateBufferBuffer(JNIEnv* env, jobject thiz, jlong addr,
                                          jlong inAddress, jint inLen,
                                          jlong outAddress, jint outLen,
                                          jint flush, jint params) {
    return Deflater_deflateCommon(env, zipStreamFromAddress(addr),
                                  inAddress ? (Bytef*)(uintptr_t)inAddress : NULL, inLen,
                                  outAddress ? (Bytef*)(uintptr_t)outAddress : NULL, outLen,
                                  flush, params);
}

static jint Deflater_getAdler(JNIEnv* env, jclass cls, jlong addr) {
    z_stream* zs = zipStreamFromAddress(addr);
    return zs ? (jint)zs->adler : 0;
}

static void Deflater_reset(JNIEnv* env, jclass cls, jlong addr) {
    z_stream* zs = zipStreamFromAddress(addr);
    if (zs) deflateReset(zs);
}

static void Deflater_end(JNIEnv* env, jclass cls, jlong addr) {
    z_stream* zs = zipStreamFromAddress(addr);
    if (zs) {
        deflateEnd(zs);
        free(zs);
    }
}

static void Inflater_initIDs(JNIEnv* env, jclass cls) { }

static jlong Inflater_init(JNIEnv* env, jclass cls, jboolean nowrap) {
    z_stream* zs = (z_stream*)calloc(1, sizeof(z_stream));
    if (!zs) {
        throwOutOfMemoryError(env, "zlib inflate stream allocation failed");
        return 0;
    }
    int windowBits = nowrap ? -MAX_WBITS : MAX_WBITS;
    int rc = inflateInit2(zs, windowBits);
    if (rc != Z_OK) {
        const char* msg = zipMessage(zs, "inflateInit2 failed");
        inflateEnd(zs);
        free(zs);
        throwZipException(env, msg);
        return 0;
    }
    return (jlong)(uintptr_t)zs;
}

static void Inflater_setDictionary(JNIEnv* env, jclass cls,
                                   jlong addr, jbyteArray array, jint off, jint len) {
    z_stream* zs = zipStreamFromAddress(addr);
    if (!zs || !array || len <= 0) return;
    jsize arrayLen = (*env)->GetArrayLength(env, array);
    if (off < 0 || len < 0 || off > arrayLen || len > arrayLen - off) return;
    jbyte* bytes = (*env)->GetByteArrayElements(env, array, NULL);
    if (!bytes) return;
    int rc = inflateSetDictionary(zs, (const Bytef*)(bytes + off), (uInt)len);
    (*env)->ReleaseByteArrayElements(env, array, bytes, JNI_ABORT);
    if (rc != Z_OK) throwDataFormatException(env, zipMessage(zs, "inflateSetDictionary failed"));
}

static void Inflater_setDictionaryBuffer(JNIEnv* env, jclass cls,
                                         jlong addr, jlong buffer, jint len) {
    z_stream* zs = zipStreamFromAddress(addr);
    if (!zs || buffer == 0 || len <= 0) return;
    int rc = inflateSetDictionary(zs, (const Bytef*)(uintptr_t)buffer, (uInt)len);
    if (rc != Z_OK) throwDataFormatException(env, zipMessage(zs, "inflateSetDictionary failed"));
}

static jlong Inflater_inflateCommon(JNIEnv* env, jobject thiz, z_stream* zs,
                                    Bytef* in, jint inLen,
                                    Bytef* out, jint outLen) {
    if (!zs) {
        throwDataFormatException(env, "inflater stream is closed");
        return 0;
    }
    if (inLen < 0) inLen = 0;
    if (outLen < 0) outLen = 0;

    uLong oldIn = zs->total_in;
    uLong oldOut = zs->total_out;
    zs->next_in = inLen > 0 ? in : Z_NULL;
    zs->avail_in = (uInt)inLen;
    zs->next_out = outLen > 0 ? out : Z_NULL;
    zs->avail_out = (uInt)outLen;

    int rc = inflate(zs, Z_PARTIAL_FLUSH);
    jint consumed = (jint)(zs->total_in - oldIn);
    jint produced = (jint)(zs->total_out - oldOut);

    if (rc == Z_DATA_ERROR || rc == Z_MEM_ERROR || rc == Z_STREAM_ERROR) {
        setIntFieldIfPresent(env, thiz, "inputConsumed", consumed);
        setIntFieldIfPresent(env, thiz, "outputConsumed", produced);
        throwDataFormatException(env, zipMessage(zs, "inflate failed"));
        return 0;
    }

    return zipPackResult(consumed, produced, rc == Z_STREAM_END, rc == Z_NEED_DICT);
}

static jlong Inflater_inflateBytesBytes(JNIEnv* env, jobject thiz, jlong addr,
                                        jbyteArray inArray, jint inOff, jint inLen,
                                        jbyteArray outArray, jint outOff, jint outLen) {
    jbyte* inBytes = NULL;
    jbyte* outBytes = NULL;
    if (inArray && inLen > 0) {
        inBytes = (*env)->GetByteArrayElements(env, inArray, NULL);
        if (!inBytes) return 0;
    }
    if (outArray && outLen > 0) {
        outBytes = (*env)->GetByteArrayElements(env, outArray, NULL);
        if (!outBytes) {
            if (inBytes) (*env)->ReleaseByteArrayElements(env, inArray, inBytes, JNI_ABORT);
            return 0;
        }
    }
    jlong result = Inflater_inflateCommon(env, thiz, zipStreamFromAddress(addr),
                                          inBytes ? (Bytef*)(inBytes + inOff) : NULL, inLen,
                                          outBytes ? (Bytef*)(outBytes + outOff) : NULL, outLen);
    if (inBytes) (*env)->ReleaseByteArrayElements(env, inArray, inBytes, JNI_ABORT);
    if (outBytes) (*env)->ReleaseByteArrayElements(env, outArray, outBytes, 0);
    return result;
}

static jlong Inflater_inflateBytesBuffer(JNIEnv* env, jobject thiz, jlong addr,
                                         jbyteArray inArray, jint inOff, jint inLen,
                                         jlong outAddress, jint outLen) {
    jbyte* inBytes = NULL;
    if (inArray && inLen > 0) {
        inBytes = (*env)->GetByteArrayElements(env, inArray, NULL);
        if (!inBytes) return 0;
    }
    jlong result = Inflater_inflateCommon(env, thiz, zipStreamFromAddress(addr),
                                          inBytes ? (Bytef*)(inBytes + inOff) : NULL, inLen,
                                          outAddress ? (Bytef*)(uintptr_t)outAddress : NULL, outLen);
    if (inBytes) (*env)->ReleaseByteArrayElements(env, inArray, inBytes, JNI_ABORT);
    return result;
}

static jlong Inflater_inflateBufferBytes(JNIEnv* env, jobject thiz, jlong addr,
                                         jlong inAddress, jint inLen,
                                         jbyteArray outArray, jint outOff, jint outLen) {
    jbyte* outBytes = NULL;
    if (outArray && outLen > 0) {
        outBytes = (*env)->GetByteArrayElements(env, outArray, NULL);
        if (!outBytes) return 0;
    }
    jlong result = Inflater_inflateCommon(env, thiz, zipStreamFromAddress(addr),
                                          inAddress ? (Bytef*)(uintptr_t)inAddress : NULL, inLen,
                                          outBytes ? (Bytef*)(outBytes + outOff) : NULL, outLen);
    if (outBytes) (*env)->ReleaseByteArrayElements(env, outArray, outBytes, 0);
    return result;
}

static jlong Inflater_inflateBufferBuffer(JNIEnv* env, jobject thiz, jlong addr,
                                          jlong inAddress, jint inLen,
                                          jlong outAddress, jint outLen) {
    return Inflater_inflateCommon(env, thiz, zipStreamFromAddress(addr),
                                  inAddress ? (Bytef*)(uintptr_t)inAddress : NULL, inLen,
                                  outAddress ? (Bytef*)(uintptr_t)outAddress : NULL, outLen);
}

static jint Inflater_getAdler(JNIEnv* env, jclass cls, jlong addr) {
    z_stream* zs = zipStreamFromAddress(addr);
    return zs ? (jint)zs->adler : 0;
}

static void Inflater_reset(JNIEnv* env, jclass cls, jlong addr) {
    z_stream* zs = zipStreamFromAddress(addr);
    if (zs) inflateReset(zs);
}

static void Inflater_end(JNIEnv* env, jclass cls, jlong addr) {
    z_stream* zs = zipStreamFromAddress(addr);
    if (zs) {
        inflateEnd(zs);
        free(zs);
    }
}

/* ==================== JNI_OnLoad ==================== */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return -1;

    /* Register native methods for libcore.io.Linux */
    jclass linuxClass = (*env)->FindClass(env, "libcore/io/Linux");
    if (linuxClass) {
        JNINativeMethod methods[] = {
            {"getpwuid", "(I)Landroid/system/StructPasswd;", (void*)linux_getpwuid},
            {"uname", "()Landroid/system/StructUtsname;", (void*)linux_uname},
            {"getenv", "(Ljava/lang/String;)Ljava/lang/String;", (void*)linux_getenv},
            {"getuid", "()I", (void*)linux_getuid},
            {"getpid", "()I", (void*)linux_getpid},
            {"geteuid", "()I", (void*)linux_geteuid},
            {"getegid", "()I", (void*)linux_getegid},
            {"getgid", "()I", (void*)linux_getgid},
            {"getppid", "()I", (void*)linux_getppid},
            {"sysconf", "(I)J", (void*)linux_sysconf},
            {"isatty", "(Ljava/io/FileDescriptor;)Z", (void*)linux_isatty},
            {"writeBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I", (void*)linux_writeBytes},
            {"readBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I", (void*)linux_readBytes},
            {"close", "(Ljava/io/FileDescriptor;)V", (void*)linux_close},
            {"open", "(Ljava/lang/String;II)Ljava/io/FileDescriptor;", (void*)linux_open},
            {"lseek", "(Ljava/io/FileDescriptor;JI)J", (void*)linux_lseek},
            {"fstat", "(Ljava/io/FileDescriptor;)Landroid/system/StructStat;", (void*)linux_fstat},
            {"stat", "(Ljava/lang/String;)Landroid/system/StructStat;", (void*)linux_stat},
            {"lstat", "(Ljava/lang/String;)Landroid/system/StructStat;", (void*)linux_lstat},
            {"access", "(Ljava/lang/String;I)Z", (void*)linux_access},
            {"environ", "()[Ljava/lang/String;", (void*)linux_environ},
            {"setenv", "(Ljava/lang/String;Ljava/lang/String;Z)V", (void*)linux_setenv},
            {"strerror", "(I)Ljava/lang/String;", (void*)linux_strerror},
            {"android_fdsan_exchange_owner_tag", "(Ljava/io/FileDescriptor;JJ)V", (void*)linux_android_fdsan_exchange_owner_tag},
            {"android_fdsan_get_owner_tag", "(Ljava/io/FileDescriptor;)J", (void*)linux_android_fdsan_get_owner_tag},
            {"android_fdsan_get_tag_type", "(J)Ljava/lang/String;", (void*)linux_android_fdsan_get_tag_type},
            {"android_fdsan_get_tag_value", "(J)J", (void*)linux_android_fdsan_get_tag_value},
            {"fcntlVoid", "(Ljava/io/FileDescriptor;I)I", (void*)linux_fcntlVoid},
            {"fcntlInt", "(Ljava/io/FileDescriptor;II)I", (void*)linux_fcntlInt},
            {"mkdir", "(Ljava/lang/String;I)V", (void*)linux_mkdir},
            {"rename", "(Ljava/lang/String;Ljava/lang/String;)V", (void*)linux_rename},
            {"remove", "(Ljava/lang/String;)V", (void*)linux_remove},
            {"unlink", "(Ljava/lang/String;)V", (void*)linux_unlink},
            {"chmod", "(Ljava/lang/String;I)V", (void*)linux_chmod},
            {"fchmod", "(Ljava/io/FileDescriptor;I)V", (void*)linux_fchmod},
            {"umaskImpl", "(I)I", (void*)linux_umask},
            {"dup", "(Ljava/io/FileDescriptor;)Ljava/io/FileDescriptor;", (void*)linux_dup},
            {"dup2", "(Ljava/io/FileDescriptor;I)Ljava/io/FileDescriptor;", (void*)linux_dup2},
            {"ftruncate", "(Ljava/io/FileDescriptor;J)V", (void*)linux_ftruncate},
            {"fsync", "(Ljava/io/FileDescriptor;)V", (void*)linux_fsync},
            {"fdatasync", "(Ljava/io/FileDescriptor;)V", (void*)linux_fdatasync},
            {"symlink", "(Ljava/lang/String;Ljava/lang/String;)V", (void*)linux_symlink},
            {"readlink", "(Ljava/lang/String;)Ljava/lang/String;", (void*)linux_readlink},
            {"realpath", "(Ljava/lang/String;)Ljava/lang/String;", (void*)linux_realpath},
            {"getxattr", "(Ljava/lang/String;Ljava/lang/String;)[B", (void*)linux_getxattr},
        };
        registerNativesOrSkip(env, linuxClass, methods, sizeof(methods)/sizeof(methods[0]));
        (*env)->DeleteLocalRef(env, linuxClass);
    }

    /* Register OsConstants.initConstants() */
    {
        jclass cls = (*env)->FindClass(env, "android/system/OsConstants");
        if (cls) {
            JNINativeMethod methods[] = {
                {"initConstants", "()V", (void*)OsConstants_initConstants},
            };
            registerNativesOrSkip(env, cls, methods, 1);
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* AsynchronousCloseMonitor */
    {
        jclass cls = (*env)->FindClass(env, "libcore/io/AsynchronousCloseMonitor");
        if (cls) {
            JNINativeMethod methods[] = {
                {"signalBlockedThreads", "(Ljava/io/FileDescriptor;)V", (void*)ACM_signalBlockedThreads},
            };
            registerNativesOrSkip(env, cls, methods, 1);
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* Android 15 libcore.math.NativeBN - OpenHarmony OpenSSL-backed BigInteger. */
    {
        jclass cls = (*env)->FindClass(env, "libcore/math/NativeBN");
        if (cls) {
            JNINativeMethod methods[] = {
                {"BN_new", "()J", (void*)LibcoreNativeBN_BN_new},
                {"BN_free", "(J)V", (void*)LibcoreNativeBN_BN_free},
                {"litEndInts2bn", "([IIZJ)V", (void*)LibcoreNativeBN_litEndInts2bn},
                {"bn2litEndInts", "(J)[I", (void*)LibcoreNativeBN_bn2litEndInts},
                {"BN_mul", "(JJJ)V", (void*)LibcoreNativeBN_BN_mul},
                {"BN_div", "(JJJJ)V", (void*)LibcoreNativeBN_BN_div},
                {"BN_mod_exp", "(JJJJ)V", (void*)LibcoreNativeBN_BN_mod_exp},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        } else {
            (*env)->ExceptionClear(env);
        }
    }

    /* Legacy java.math.NativeBN - BigInteger/BigDecimal support */
    {
        jclass cls = (*env)->FindClass(env, "java/math/NativeBN");
        if (cls) {
            JNINativeMethod methods[] = {
                {"getNativeFinalizer", "()J", (void*)NativeBN_getNativeFinalizer},
                {"BN_new", "()J", (void*)NativeBN_BN_new},
                {"BN_free", "(J)V", (void*)NativeBN_BN_free},
                {"BN_cmp", "(JJ)I", (void*)NativeBN_BN_cmp},
                {"BN_copy", "(JJ)V", (void*)NativeBN_BN_copy},
                {"putLongInt", "(JJ)V", (void*)NativeBN_putLongInt},
                {"putULongInt", "(JJZ)V", (void*)NativeBN_putULongInt},
                {"BN_dec2bn", "(JLjava/lang/String;)I", (void*)NativeBN_BN_dec2bn},
                {"BN_hex2bn", "(JLjava/lang/String;)I", (void*)NativeBN_BN_hex2bn},
                {"BN_bin2bn", "([BIZJ)V", (void*)NativeBN_BN_bin2bn},
                {"litEndInts2bn", "([IIZJ)V", (void*)NativeBN_litEndInts2bn},
                {"twosComp2bn", "([BIJ)V", (void*)NativeBN_twosComp2bn},
                {"longInt", "(J)J", (void*)NativeBN_longInt},
                {"BN_bn2dec", "(J)Ljava/lang/String;", (void*)NativeBN_BN_bn2dec},
                {"BN_bn2hex", "(J)Ljava/lang/String;", (void*)NativeBN_BN_bn2hex},
                {"BN_bn2bin", "(J)[B", (void*)NativeBN_BN_bn2bin},
                {"bn2litEndInts", "(J)[I", (void*)NativeBN_bn2litEndInts},
                {"sign", "(J)I", (void*)NativeBN_sign},
                {"BN_set_negative", "(JI)V", (void*)NativeBN_BN_set_negative},
                {"bitLength", "(J)I", (void*)NativeBN_bitLength},
                {"BN_is_bit_set", "(JI)Z", (void*)NativeBN_BN_is_bit_set},
                {"BN_shift", "(JJI)V", (void*)NativeBN_BN_shift},
                {"BN_add_word", "(JI)V", (void*)NativeBN_BN_add_word},
                {"BN_mul_word", "(JI)V", (void*)NativeBN_BN_mul_word},
                {"BN_mod_word", "(JI)I", (void*)NativeBN_BN_mod_word},
                {"BN_add", "(JJJ)V", (void*)NativeBN_BN_add},
                {"BN_sub", "(JJJ)V", (void*)NativeBN_BN_sub},
                {"BN_mul", "(JJJ)V", (void*)NativeBN_BN_mul},
                {"BN_div", "(JJJJ)V", (void*)NativeBN_BN_div},
                {"BN_exp", "(JJJ)V", (void*)NativeBN_BN_exp},
                {"BN_gcd", "(JJJ)V", (void*)NativeBN_BN_gcd},
                {"BN_nnmod", "(JJJ)V", (void*)NativeBN_BN_nnmod},
                {"BN_mod_exp", "(JJJJ)V", (void*)NativeBN_BN_mod_exp},
                {"BN_mod_inverse", "(JJJ)V", (void*)NativeBN_BN_mod_inverse},
                {"BN_generate_prime_ex", "(JIZJJ)V", (void*)NativeBN_BN_generate_prime_ex},
                {"BN_primality_test", "(JIZ)Z", (void*)NativeBN_BN_primality_test},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* libcore.io.Memory - direct memory access for NIO/mmap */
    {
        jclass cls = (*env)->FindClass(env, "libcore/io/Memory");
        if (cls) {
            JNINativeMethod methods[] = {
                {"peekByte", "(J)B", (void*)Memory_peekByte},
                {"pokeByte", "(JB)V", (void*)Memory_pokeByte},
                {"peekIntNative", "(J)I", (void*)Memory_peekIntNative},
                {"pokeIntNative", "(JI)V", (void*)Memory_pokeIntNative},
                {"peekLongNative", "(J)J", (void*)Memory_peekLongNative},
                {"pokeLongNative", "(JJ)V", (void*)Memory_pokeLongNative},
                {"peekShortNative", "(J)S", (void*)Memory_peekShortNative},
                {"pokeShortNative", "(JS)V", (void*)Memory_pokeShortNative},
                {"peekByteArray", "(J[BII)V", (void*)Memory_peekByteArray},
                {"peekCharArray", "(J[CIIZ)V", (void*)Memory_peekCharArray},
                {"peekIntArray", "(J[IIIZ)V", (void*)Memory_peekIntArray},
                {"peekShortArray", "(J[SIIZ)V", (void*)Memory_peekShortArray},
                {"peekLongArray", "(J[JIIZ)V", (void*)Memory_peekLongArray},
                {"peekFloatArray", "(J[FIIZ)V", (void*)Memory_peekFloatArray},
                {"peekDoubleArray", "(J[DIIZ)V", (void*)Memory_peekDoubleArray},
                {"memmove", "(Ljava/lang/Object;ILjava/lang/Object;IJ)V", (void*)Memory_memmove},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* ICU - initLocaleDataNative (required for NumberFormat, Currency, etc.) */
    {
        jclass cls = (*env)->FindClass(env, "libcore/icu/ICU");
        if (cls) {
            JNINativeMethod methods[] = {
                {"initLocaleDataNative", "(Ljava/lang/String;Llibcore/icu/LocaleData;)Z",
                 (void*)ICU_initLocaleDataNative},
                {"getBestDateTimePatternNative", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                 (void*)ICU_getBestDateTimePatternNative},
                {"getCurrencyCode", "(Ljava/lang/String;)Ljava/lang/String;",
                 (void*)ICU_getCurrencyCode},
                {"getISO3Country", "(Ljava/lang/String;)Ljava/lang/String;",
                 (void*)ICU_getISO3Country},
                {"getISO3Language", "(Ljava/lang/String;)Ljava/lang/String;",
                 (void*)ICU_getISO3Language},
                {"getScript", "(Ljava/lang/String;)Ljava/lang/String;",
                 (void*)ICU_getScript},
                {"getISOLanguagesNative", "()[Ljava/lang/String;",
                 (void*)ICU_getISOLanguagesNative},
                {"getISOCountriesNative", "()[Ljava/lang/String;",
                 (void*)ICU_getISOCountriesNative},
                {"getAvailableLocalesNative", "()[Ljava/lang/String;",
                 (void*)ICU_getAvailableLocalesNative},
                {"setDefaultLocale", "(Ljava/lang/String;)V",
                 (void*)ICU_setDefaultLocale},
                {"getDefaultLocale", "()Ljava/lang/String;",
                 (void*)ICU_getDefaultLocale},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.lang.Thread — nicenessForPriority (Android 14+) */
    {
        jclass cls = (*env)->FindClass(env, "java/lang/Thread");
        if (cls) {
            JNINativeMethod methods[] = {
                {"nicenessForPriority", "(I)I", (void*)Thread_nicenessForPriority},
            };
            registerNativesOrSkip(env, cls, methods, 1);
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* sun.nio.ch.FileKey */
    {
        jclass cls = (*env)->FindClass(env, "sun/nio/ch/FileKey");
        if (cls) {
            JNINativeMethod methods[] = {
                {"initIDs", "()V", (void*)FileKey_initIDs},
                {"init", "(Ljava/io/FileDescriptor;)V", (void*)FileKey_init},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.util.zip.CRC32 */
    {
        jclass cls = (*env)->FindClass(env, "java/util/zip/CRC32");
        if (cls) {
            JNINativeMethod methods[] = {
                {"update", "(II)I", (void*)CRC32_update},
                {"updateBytes", "(I[BII)I", (void*)CRC32_updateBytes},
                {"updateBytes0", "(I[BII)I", (void*)CRC32_updateBytes},
                {"updateByteBuffer", "(IJII)I", (void*)CRC32_updateByteBuffer},
                {"updateByteBuffer0", "(IJII)I", (void*)CRC32_updateByteBuffer},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.util.zip.Adler32 */
    {
        jclass cls = (*env)->FindClass(env, "java/util/zip/Adler32");
        if (cls) {
            JNINativeMethod methods[] = {
                {"update", "(II)I", (void*)Adler32_update},
                {"updateBytes", "(I[BII)I", (void*)Adler32_updateBytes},
                {"updateByteBuffer", "(IJII)I", (void*)Adler32_updateByteBuffer},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.util.zip.Deflater */
    {
        jclass cls = (*env)->FindClass(env, "java/util/zip/Deflater");
        if (cls) {
            JNINativeMethod methods[] = {
                {"init", "(IIZ)J", (void*)Deflater_init},
                {"setDictionary", "(J[BII)V", (void*)Deflater_setDictionary},
                {"setDictionaryBuffer", "(JJI)V", (void*)Deflater_setDictionaryBuffer},
                {"deflateBytesBytes", "(J[BII[BIIII)J", (void*)Deflater_deflateBytesBytes},
                {"deflateBytesBuffer", "(J[BIIJIII)J", (void*)Deflater_deflateBytesBuffer},
                {"deflateBufferBytes", "(JJI[BIIII)J", (void*)Deflater_deflateBufferBytes},
                {"deflateBufferBuffer", "(JJIJIII)J", (void*)Deflater_deflateBufferBuffer},
                {"getAdler", "(J)I", (void*)Deflater_getAdler},
                {"reset", "(J)V", (void*)Deflater_reset},
                {"end", "(J)V", (void*)Deflater_end},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* java.util.zip.Inflater */
    {
        jclass cls = (*env)->FindClass(env, "java/util/zip/Inflater");
        if (cls) {
            JNINativeMethod methods[] = {
                {"initIDs", "()V", (void*)Inflater_initIDs},
                {"init", "(Z)J", (void*)Inflater_init},
                {"setDictionary", "(J[BII)V", (void*)Inflater_setDictionary},
                {"setDictionaryBuffer", "(JJI)V", (void*)Inflater_setDictionaryBuffer},
                {"inflateBytesBytes", "(J[BII[BII)J", (void*)Inflater_inflateBytesBytes},
                {"inflateBytesBuffer", "(J[BIIJI)J", (void*)Inflater_inflateBytesBuffer},
                {"inflateBufferBytes", "(JJI[BII)J", (void*)Inflater_inflateBufferBytes},
                {"inflateBufferBuffer", "(JJIJI)J", (void*)Inflater_inflateBufferBuffer},
                {"getAdler", "(J)I", (void*)Inflater_getAdler},
                {"reset", "(J)V", (void*)Inflater_reset},
                {"end", "(J)V", (void*)Inflater_end},
            };
            registerNativesOrSkip(env, cls, methods, sizeof(methods)/sizeof(methods[0]));
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    return JNI_VERSION_1_6;
}
