/*
 * OHBridge JNI stub — stdout pipe display list mode.
 * Writes Canvas ops to local buffer, flushes as [4-byte LE size][ops] to pipe.
 * Host reads from process.inputStream → replays on SurfaceView.
 *
 * On init, saves stdout fd for binary pipe and redirects stdout→stderr
 * so Java System.out and stray printf don't corrupt the binary stream.
 */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

/* WebP decoder using libwebp */
#include "webp/decode.h"

static unsigned char* decode_webp(const unsigned char* data, int len, int* w, int* h) {
    if (len < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0)
        return NULL;
    return WebPDecodeRGBA(data, len, w, h);
}
#include <pthread.h>

/* SIGBUS handler — dump Java stack trace before dying */
static JavaVM* sigbus_vm = NULL;
pthread_t __ohbridge_main_thread = 0;
static void sigbus_handler(int sig, siginfo_t* info, void* ucontext) {
    ucontext_t* uc = (ucontext_t*)ucontext;
    fprintf(stderr, "\n[OHBridge] SIGBUS caught! fault_addr=%p\n", info->si_addr);
#ifdef __aarch64__
    fprintf(stderr, "[OHBridge]   pc=%p  lr=%p  x16=%p\n",
        (void*)uc->uc_mcontext.pc,
        (void*)uc->uc_mcontext.regs[30],
        (void*)uc->uc_mcontext.regs[16]);
#endif
    /* Dump ArtMethod fields at x16 (entry_point is at offset 24 on 64-bit) */
#ifdef __aarch64__
    {
        unsigned char* method = (unsigned char*)(uintptr_t)uc->uc_mcontext.regs[16];
        if (method) {
            uint32_t access_flags = *(uint32_t*)(method + 4);
            uint32_t dex_method_idx = *(uint32_t*)(method + 12);
            void* data = *(void**)(method + 16);
            void* entry = *(void**)(method + 24);
            fprintf(stderr, "[OHBridge] ArtMethod@%p: access=0x%x dex_idx=%u data=%p entry=%p\n",
                method, access_flags, dex_method_idx, data, entry);
            /* Also try offset 32 in case layout differs */
            void* entry32 = *(void**)(method + 32);
            fprintf(stderr, "[OHBridge] ArtMethod alt offsets: [32]=%p [40]=%p\n",
                entry32, *(void**)(method + 40));
        }
    }
#endif
    /* Try to dump Java stack */
    if (sigbus_vm) {
        JNIEnv* env = NULL;
        if ((*sigbus_vm)->GetEnv(sigbus_vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK && env) {
            jclass threadCls = (*env)->FindClass(env, "java/lang/Thread");
            if (threadCls) {
                jmethodID currentThread = (*env)->GetStaticMethodID(env, threadCls, "currentThread", "()Ljava/lang/Thread;");
                if (currentThread) {
                    jobject thread = (*env)->CallStaticObjectMethod(env, threadCls, currentThread);
                    if (thread) {
                        jmethodID dumpStack = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, thread), "getStackTrace", "()[Ljava/lang/StackTraceElement;");
                        if (dumpStack) {
                            jobjectArray frames = (jobjectArray)(*env)->CallObjectMethod(env, thread, dumpStack);
                            if (frames) {
                                int len = (*env)->GetArrayLength(env, frames);
                                fprintf(stderr, "[OHBridge] Java stack (%d frames):\n", len);
                                jmethodID toStr = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/StackTraceElement"), "toString", "()Ljava/lang/String;");
                                for (int i = 0; i < len && i < 20; i++) {
                                    jobject frame = (*env)->GetObjectArrayElement(env, frames, i);
                                    if (frame && toStr) {
                                        jstring s = (jstring)(*env)->CallObjectMethod(env, frame, toStr);
                                        const char* cs = (*env)->GetStringUTFChars(env, s, NULL);
                                        fprintf(stderr, "[OHBridge]   at %s\n", cs);
                                        (*env)->ReleaseStringUTFChars(env, s, cs);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    fflush(stderr);
    /* Re-raise with default handler — let the OS handle the crash.
     * We logged the diagnostic info above. */
    fflush(stderr);
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
    raise(sig);
}

#define DLIST_MAX (2*1024*1024) /* 2MB — enough for decoded ARGB bitmaps */

static unsigned char dlist_buf[DLIST_MAX];
static int dlist_pos = 0;
static int pipe_fd = -1;   /* saved stdout fd for binary pipe */
static const int DLIST_MAGIC = 0x444C5354; /* "DLST" */
static JavaVM* g_vm = NULL;

static void emit1(unsigned char v) { if(dlist_pos<DLIST_MAX-64) dlist_buf[dlist_pos++]=v; }
static void emit4(const void* v) { if(dlist_pos+4<=DLIST_MAX-64){memcpy(dlist_buf+dlist_pos,v,4);dlist_pos+=4;} }
static void emitf(float v) { emit4(&v); }
static void emiti(int v) { emit4(&v); }
static void emit2(short v) { if(dlist_pos+2<=DLIST_MAX-64){memcpy(dlist_buf+dlist_pos,&v,2);dlist_pos+=2;} }

enum { OP_COLOR=1,OP_RECT=2,OP_TEXT=3,OP_LINE=4,OP_SAVE=5,OP_RESTORE=6,OP_TRANSLATE=7,OP_CLIP=8,OP_RRECT=9,OP_CIRCLE=10,OP_IMAGE=11,OP_ARGB_BITMAP=12 };

#define MAX_H 256
static int h_colors[MAX_H];
static float h_fontsz[MAX_H];
static int h_next = 1;
static int idx(long h) { return (int)(h & 0xFF); }

/* Write all bytes to fd, handling partial writes */
static void write_all(int fd, const void* buf, int len) {
    const unsigned char* p = (const unsigned char*)buf;
    while (len > 0) {
        int n = write(fd, p, len);
        if (n <= 0) break;
        p += n;
        len -= n;
    }
}

/* === JNI exports (Java_com_ohos_shim_bridge_OHBridge_*) === */
#define JF(name) Java_com_ohos_shim_bridge_OHBridge_##name

JNIEXPORT jint JNICALL JF(arkuiInit)(JNIEnv* e, jclass c) {
    fprintf(stderr, "[OHBridge] pipe mode arkuiInit (pipe_fd=%d)\n", pipe_fd);
    return 0;
}
JNIEXPORT jlong JNICALL JF(surfaceCreate)(JNIEnv* e, jclass c, jlong u, jint w, jint h) { return 1; }
JNIEXPORT jlong JNICALL JF(surfaceGetCanvas)(JNIEnv* e, jclass c, jlong s) { dlist_pos=0; return 1; }
JNIEXPORT jint JNICALL JF(surfaceFlush)(JNIEnv* e, jclass c, jlong s) {
    if(pipe_fd<0) return -1;
    int size = dlist_pos;
    /* Count images by scanning properly from known start positions */
    int img_count = 0;
    /* Simple scan: just count first-position ops, don't try to parse mid-stream */
    {
        int i = 0;
        while (i < size) {
            int op = dlist_buf[i] & 0xFF;
            if (op == OP_COLOR) { i += 5; img_count++; continue; }  /* 1+4 */
            if (op == OP_RECT) { i += 17; continue; }   /* 1+4*4 */
            if (op == OP_SAVE) { i += 1; continue; }
            if (op == OP_RESTORE) { i += 1; continue; }
            if (op == OP_TRANSLATE) { i += 9; continue; } /* 1+4+4 */
            if (op == OP_CLIP) { i += 17; continue; }  /* 1+4*4 */
            if ((op == OP_IMAGE || op == OP_ARGB_BITMAP) && i + 21 <= size) {
                int dlen = *(int*)(dlist_buf + i + 17);
                if (dlen >= 0 && dlen <= size - i - 21) {
                    img_count++;
                    i += 21 + dlen;
                    continue;
                }
            }
            i++; /* unknown op or corrupt — skip byte */
        }
    }
    fprintf(stderr, "[OHBridge] surfaceFlush: %d bytes, %d images, pipe_fd=%d\n", size, img_count, pipe_fd);
    dlist_pos = 0;
    write_all(pipe_fd, &DLIST_MAGIC, 4);
    write_all(pipe_fd, &size, 4);
    write_all(pipe_fd, dlist_buf, size);
    return 0;
}
JNIEXPORT void JNICALL JF(surfaceDestroy)(JNIEnv* e, jclass c, jlong s) {}
JNIEXPORT void JNICALL JF(surfaceResize)(JNIEnv* e, jclass c, jlong s, jint w, jint h) {}

JNIEXPORT jlong JNICALL JF(canvasCreate)(JNIEnv* e, jclass c, jlong b) { return 1; }
JNIEXPORT void JNICALL JF(canvasDestroy)(JNIEnv* e, jclass c, jlong cn) {}
JNIEXPORT void JNICALL JF(canvasDrawColor)(JNIEnv* e, jclass c, jlong cn, jint col) { emit1(OP_COLOR); emiti(col); }
JNIEXPORT void JNICALL JF(canvasDrawRect)(JNIEnv* e, jclass c, jlong cn, jfloat l, jfloat t, jfloat r, jfloat b2, jlong pen, jlong brush) {
    emit1(OP_RECT); emitf(l); emitf(t); emitf(r); emitf(b2); emiti(h_colors[idx(brush>0?brush:pen)]);
}
JNIEXPORT void JNICALL JF(canvasDrawRoundRect)(JNIEnv* e, jclass c, jlong cn, jfloat l, jfloat t, jfloat r, jfloat b2, jfloat rx, jfloat ry, jlong pen, jlong brush) {
    emit1(OP_RRECT); emitf(l); emitf(t); emitf(r); emitf(b2); emitf(rx); emitf(ry); emiti(h_colors[idx(brush>0?brush:pen)]);
}
JNIEXPORT void JNICALL JF(canvasDrawCircle)(JNIEnv* e, jclass c, jlong cn, jfloat cx, jfloat cy, jfloat r, jlong pen, jlong brush) {
    emit1(OP_CIRCLE); emitf(cx); emitf(cy); emitf(r); emiti(h_colors[idx(brush>0?brush:pen)]);
}
JNIEXPORT void JNICALL JF(canvasDrawLine)(JNIEnv* e, jclass c, jlong cn, jfloat x1, jfloat y1, jfloat x2, jfloat y2, jlong pen) {
    emit1(OP_LINE); emitf(x1); emitf(y1); emitf(x2); emitf(y2); emiti(h_colors[idx(pen)]); emitf(1.0f);
}
JNIEXPORT void JNICALL JF(canvasDrawText)(JNIEnv* e, jclass c, jlong cn, jstring text, jfloat x, jfloat y, jlong font, jlong pen, jlong brush) {
    if(!text) return;
    const char* u = (*e)->GetStringUTFChars(e,text,0);
    int len = u ? strlen(u) : 0;
    if(len>0 && dlist_pos+19+len<DLIST_MAX-64) {
        emit1(OP_TEXT); emitf(x); emitf(y); emitf(h_fontsz[idx(font)]);
        emiti(h_colors[idx(pen>0?pen:brush)]); emit2((short)len);
        memcpy(dlist_buf+dlist_pos,u,len); dlist_pos+=len;
    }
    if(u) (*e)->ReleaseStringUTFChars(e,text,u);
}
JNIEXPORT void JNICALL JF(canvasSave)(JNIEnv* e, jclass c, jlong cn) { emit1(OP_SAVE); }
JNIEXPORT void JNICALL JF(canvasRestore)(JNIEnv* e, jclass c, jlong cn) { emit1(OP_RESTORE); }
JNIEXPORT void JNICALL JF(canvasTranslate)(JNIEnv* e, jclass c, jlong cn, jfloat dx, jfloat dy) { emit1(OP_TRANSLATE); emitf(dx); emitf(dy); }
JNIEXPORT void JNICALL JF(canvasScale)(JNIEnv* e, jclass c, jlong cn, jfloat sx, jfloat sy) {}
JNIEXPORT void JNICALL JF(canvasClipRect)(JNIEnv* e, jclass c, jlong cn, jfloat l, jfloat t, jfloat r, jfloat b2) { emit1(OP_CLIP); emitf(l); emitf(t); emitf(r); emitf(b2); }
JNIEXPORT void JNICALL JF(canvasDrawPath)(JNIEnv* e, jclass c, jlong cn, jlong path, jlong pen, jlong brush) {}
JNIEXPORT void JNICALL JF(canvasDrawBitmap)(JNIEnv* e, jclass c, jlong cn, jlong bmp, jfloat x, jfloat y) {
    /* Legacy: no-op for native-handle bitmaps in pipe mode */
}

/* Draw raw image bytes (PNG/JPEG/WebP) — emits OP_IMAGE for host-side decoding */
JNIEXPORT void JNICALL JF(canvasDrawImage)(JNIEnv* e, jclass c, jlong cn, jbyteArray data, jfloat x, jfloat y, jint w, jint h) {
    if (!data) return;
    jint len = (*e)->GetArrayLength(e, data);
    if (len <= 0 || dlist_pos + 21 + len > DLIST_MAX - 64) return;
    emit1(OP_IMAGE);
    emitf(x); emitf(y);
    emiti(w); emiti(h);
    emiti(len);
    jbyte* bytes = (*e)->GetByteArrayElements(e, data, NULL);
    if (bytes) {
        memcpy(dlist_buf + dlist_pos, bytes, len);
        dlist_pos += len;
        (*e)->ReleaseByteArrayElements(e, data, bytes, JNI_ABORT);
    }
}
JNIEXPORT void JNICALL JF(canvasConcat)(JNIEnv* e, jclass c, jlong cn, jfloatArray m) {}
JNIEXPORT void JNICALL JF(canvasRotate)(JNIEnv* e, jclass c, jlong cn, jfloat d, jfloat px, jfloat py) {}
JNIEXPORT void JNICALL JF(canvasClipPath)(JNIEnv* e, jclass c, jlong cn, jlong path) {}
JNIEXPORT void JNICALL JF(canvasDrawArc)(JNIEnv* e, jclass c, jlong cn, jfloat l, jfloat t, jfloat r, jfloat b2, jfloat sa, jfloat sw, jboolean uc, jlong pen, jlong brush) {}
JNIEXPORT void JNICALL JF(canvasDrawOval)(JNIEnv* e, jclass c, jlong cn, jfloat l, jfloat t, jfloat r, jfloat b2, jlong pen, jlong brush) {}

JNIEXPORT jlong JNICALL JF(penCreate)(JNIEnv* e, jclass c) { int i=h_next++; if(i>=MAX_H)i=h_next=1; h_colors[i]=0xFF000000; return i; }
JNIEXPORT void JNICALL JF(penSetColor)(JNIEnv* e, jclass c, jlong p, jint col) { h_colors[idx(p)]=col; }
JNIEXPORT void JNICALL JF(penSetWidth)(JNIEnv* e, jclass c, jlong p, jfloat w) {}
JNIEXPORT void JNICALL JF(penSetAntiAlias)(JNIEnv* e, jclass c, jlong p, jboolean aa) {}
JNIEXPORT void JNICALL JF(penSetCap)(JNIEnv* e, jclass c, jlong p, jint cap) {}
JNIEXPORT void JNICALL JF(penSetJoin)(JNIEnv* e, jclass c, jlong p, jint j) {}
JNIEXPORT void JNICALL JF(penDestroy)(JNIEnv* e, jclass c, jlong p) {}
JNIEXPORT jlong JNICALL JF(brushCreate)(JNIEnv* e, jclass c) { int i=h_next++; if(i>=MAX_H)i=h_next=1; h_colors[i]=0xFF000000; return i; }
JNIEXPORT void JNICALL JF(brushSetColor)(JNIEnv* e, jclass c, jlong b, jint col) { h_colors[idx(b)]=col; }
JNIEXPORT void JNICALL JF(brushDestroy)(JNIEnv* e, jclass c, jlong b) {}
JNIEXPORT void JNICALL JF(brushSetAntiAlias)(JNIEnv* e, jclass c, jlong b, jboolean aa) {}

JNIEXPORT jlong JNICALL JF(fontCreate)(JNIEnv* e, jclass c) { int i=h_next++; if(i>=MAX_H)i=h_next=1; h_fontsz[i]=16.0f; return i; }
JNIEXPORT void JNICALL JF(fontSetSize)(JNIEnv* e, jclass c, jlong f, jfloat sz) { h_fontsz[idx(f)]=sz; }
JNIEXPORT jfloat JNICALL JF(fontMeasureText)(JNIEnv* e, jclass c, jlong f, jstring s) {
    if(!s) return 0;
    const char* u=(*e)->GetStringUTFChars(e,s,0);
    float w=u?strlen(u)*h_fontsz[idx(f)]*0.55f:0;
    if(u)(*e)->ReleaseStringUTFChars(e,s,u);
    return w;
}
JNIEXPORT void JNICALL JF(fontDestroy)(JNIEnv* e, jclass c, jlong f) {}
JNIEXPORT jfloatArray JNICALL JF(fontGetMetrics)(JNIEnv* e, jclass c, jlong f) {
    jfloatArray a=(*e)->NewFloatArray(e,4);
    float s=h_fontsz[idx(f)], m[4]={-s*0.8f,s*0.2f,0,s};
    (*e)->SetFloatArrayRegion(e,a,0,4,m);
    return a;
}

/* Emit OP_ARGB_BITMAP: [op(1)][x(4f)][y(4f)][w(4i)][h(4i)][dataLen(4i)][pixels(dataLen)] */
JNIEXPORT void JNICALL JF(canvasDrawArgbBitmap)(JNIEnv* e, jclass c, jlong canvas,
    jintArray pixelArray, jfloat x, jfloat y, jint w, jint h) {
    if (!pixelArray || w <= 0 || h <= 0) return;
    int pixCount = w * h;
    int dataLen = pixCount * 4;
    int headerSize = 1 + 4 + 4 + 4 + 4 + 4; /* op + x + y + w + h + dataLen */
    if (dlist_pos + headerSize + dataLen > DLIST_MAX - 256) {
        fprintf(stderr, "[OHBridge] canvasDrawArgbBitmap: skipping %dx%d (%d bytes, would overflow dlist)\n", w, h, dataLen);
        return; /* too large for display list buffer */
    }
    emit1(OP_ARGB_BITMAP);
    emitf(x); emitf(y);
    emiti(w); emiti(h);
    emiti(dataLen);
    jint* pixels = (*e)->GetIntArrayElements(e, pixelArray, NULL);
    /* Convert ARGB ints to RGBA bytes */
    for (int i = 0; i < pixCount; i++) {
        int argb = pixels[i];
        unsigned char a = (argb >> 24) & 0xFF;
        unsigned char r = (argb >> 16) & 0xFF;
        unsigned char g = (argb >> 8) & 0xFF;
        unsigned char b = argb & 0xFF;
        if (dlist_pos + 4 <= DLIST_MAX - 64) {
            dlist_buf[dlist_pos++] = r;
            dlist_buf[dlist_pos++] = g;
            dlist_buf[dlist_pos++] = b;
            dlist_buf[dlist_pos++] = a;
        }
    }
    (*e)->ReleaseIntArrayElements(e, pixelArray, pixels, JNI_ABORT);
}

JNIEXPORT jlong JNICALL JF(bitmapCreate)(JNIEnv* e, jclass c, jint w, jint h, jint fmt) { return 1; }
JNIEXPORT void JNICALL JF(bitmapDestroy)(JNIEnv* e, jclass c, jlong b) {}
JNIEXPORT jint JNICALL JF(bitmapGetWidth)(JNIEnv* e, jclass c, jlong b) { return 480; }
JNIEXPORT jint JNICALL JF(bitmapGetHeight)(JNIEnv* e, jclass c, jlong b) { return 800; }
JNIEXPORT void JNICALL JF(bitmapSetPixel)(JNIEnv* e, jclass c, jlong b, jint x, jint y, jint col) {}
JNIEXPORT jint JNICALL JF(bitmapGetPixel)(JNIEnv* e, jclass c, jlong b, jint x, jint y) { return 0; }

/*
 * Decode PNG/JPEG image bytes → ARGB int array (for BitmapFactory).
 * Returns int[] with [width, height, pixel0, pixel1, ...] or null on failure.
 * Pixels are ARGB_8888 format (matching Android Bitmap).
 */
JNIEXPORT jintArray JNICALL JF(imageDecodeToPixels)(JNIEnv* e, jclass c, jbyteArray data) {
    if (!data) return NULL;
    jsize len = (*e)->GetArrayLength(e, data);
    jbyte* bytes = (*e)->GetByteArrayElements(e, data, NULL);
    if (!bytes) return NULL;

    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory((unsigned char*)bytes, len, &w, &h, &channels, 4);
    if (!pixels) {
        /* Try WebP decode */
        pixels = decode_webp((unsigned char*)bytes, len, &w, &h);
    }
    (*e)->ReleaseByteArrayElements(e, data, bytes, JNI_ABORT);

    if (!pixels) {
        fprintf(stderr, "[OHBridge] imageDecodeToPixels: decode failed: %s\n", stbi_failure_reason());
        return NULL;
    }

    /* Convert RGBA → ARGB and pack into int array: [w, h, pixels...] */
    int pixelCount = w * h;
    jintArray result = (*e)->NewIntArray(e, 2 + pixelCount);
    if (!result) { stbi_image_free(pixels); return NULL; }

    jint* out = (*e)->GetIntArrayElements(e, result, NULL);
    out[0] = w;
    out[1] = h;
    for (int i = 0; i < pixelCount; i++) {
        unsigned char r = pixels[i*4+0];
        unsigned char g = pixels[i*4+1];
        unsigned char b = pixels[i*4+2];
        unsigned char a = pixels[i*4+3];
        out[2+i] = (a << 24) | (r << 16) | (g << 8) | b; /* ARGB */
    }
    (*e)->ReleaseIntArrayElements(e, result, out, 0);
    stbi_image_free(pixels);

    fprintf(stderr, "[OHBridge] imageDecodeToPixels: %dx%d (%d bytes)\n", w, h, len);
    return result;
}

JNIEXPORT jlong JNICALL JF(pathCreate)(JNIEnv* e, jclass c) { return 1; }
JNIEXPORT void JNICALL JF(pathDestroy)(JNIEnv* e, jclass c, jlong p) {}
JNIEXPORT void JNICALL JF(pathMoveTo)(JNIEnv* e, jclass c, jlong p, jfloat x, jfloat y) {}
JNIEXPORT void JNICALL JF(pathLineTo)(JNIEnv* e, jclass c, jlong p, jfloat x, jfloat y) {}
JNIEXPORT void JNICALL JF(pathClose)(JNIEnv* e, jclass c, jlong p) {}
JNIEXPORT void JNICALL JF(pathReset)(JNIEnv* e, jclass c, jlong p) {}
JNIEXPORT void JNICALL JF(pathQuadTo)(JNIEnv* e, jclass c, jlong p, jfloat x1, jfloat y1, jfloat x2, jfloat y2) {}
JNIEXPORT void JNICALL JF(pathCubicTo)(JNIEnv* e, jclass c, jlong p, jfloat x1, jfloat y1, jfloat x2, jfloat y2, jfloat x3, jfloat y3) {}
JNIEXPORT void JNICALL JF(pathAddRect)(JNIEnv* e, jclass c, jlong p, jfloat l, jfloat t, jfloat r, jfloat b, jint dir) {}
JNIEXPORT void JNICALL JF(pathAddCircle)(JNIEnv* e, jclass c, jlong p, jfloat cx, jfloat cy, jfloat r, jint dir) {}

/* === Logging & device info stubs === */
JNIEXPORT void JNICALL JF(logDebug)(JNIEnv* e, jclass c, jstring tag, jstring msg) {
    if(!tag||!msg) return;
    const char* t=(*e)->GetStringUTFChars(e,tag,0);
    const char* m=(*e)->GetStringUTFChars(e,msg,0);
    fprintf(stderr,"D/%s: %s\n",t?t:"?",m?m:"");
    if(t)(*e)->ReleaseStringUTFChars(e,tag,t);
    if(m)(*e)->ReleaseStringUTFChars(e,msg,m);
}
JNIEXPORT void JNICALL JF(logInfo)(JNIEnv* e, jclass c, jstring tag, jstring msg) {
    if(!tag||!msg) return;
    const char* t=(*e)->GetStringUTFChars(e,tag,0);
    const char* m=(*e)->GetStringUTFChars(e,msg,0);
    fprintf(stderr,"I/%s: %s\n",t?t:"?",m?m:"");
    if(t)(*e)->ReleaseStringUTFChars(e,tag,t);
    if(m)(*e)->ReleaseStringUTFChars(e,msg,m);
}
JNIEXPORT void JNICALL JF(logError)(JNIEnv* e, jclass c, jstring tag, jstring msg) {
    if(!tag||!msg) return;
    const char* t=(*e)->GetStringUTFChars(e,tag,0);
    const char* m=(*e)->GetStringUTFChars(e,msg,0);
    fprintf(stderr,"E/%s: %s\n",t?t:"?",m?m:"");
    if(t)(*e)->ReleaseStringUTFChars(e,tag,t);
    if(m)(*e)->ReleaseStringUTFChars(e,msg,m);
}
JNIEXPORT jstring JNICALL JF(getDeviceBrand)(JNIEnv* e, jclass c) { return (*e)->NewStringUTF(e,"Westlake"); }
JNIEXPORT jstring JNICALL JF(getDeviceModel)(JNIEnv* e, jclass c) { return (*e)->NewStringUTF(e,"VM"); }
JNIEXPORT jstring JNICALL JF(getOSVersion)(JNIEnv* e, jclass c) { return (*e)->NewStringUTF(e,"11"); }
JNIEXPORT jint JNICALL JF(getSDKVersion)(JNIEnv* e, jclass c) { return 30; }

/* === Registration table === */
static JNINativeMethod methods[] = {
    {"arkuiInit","()I",(void*)JF(arkuiInit)},
    {"logDebug","(Ljava/lang/String;Ljava/lang/String;)V",(void*)JF(logDebug)},
    {"logInfo","(Ljava/lang/String;Ljava/lang/String;)V",(void*)JF(logInfo)},
    {"logError","(Ljava/lang/String;Ljava/lang/String;)V",(void*)JF(logError)},
    {"getDeviceBrand","()Ljava/lang/String;",(void*)JF(getDeviceBrand)},
    {"getDeviceModel","()Ljava/lang/String;",(void*)JF(getDeviceModel)},
    {"getOSVersion","()Ljava/lang/String;",(void*)JF(getOSVersion)},
    {"getSDKVersion","()I",(void*)JF(getSDKVersion)},
    {"surfaceCreate","(JII)J",(void*)JF(surfaceCreate)},
    {"surfaceGetCanvas","(J)J",(void*)JF(surfaceGetCanvas)},
    {"surfaceFlush","(J)I",(void*)JF(surfaceFlush)},
    {"surfaceDestroy","(J)V",(void*)JF(surfaceDestroy)},
    {"surfaceResize","(JII)V",(void*)JF(surfaceResize)},
    {"canvasCreate","(J)J",(void*)JF(canvasCreate)},{"canvasDestroy","(J)V",(void*)JF(canvasDestroy)},
    {"canvasDrawColor","(JI)V",(void*)JF(canvasDrawColor)},
    {"canvasDrawRect","(JFFFFJJ)V",(void*)JF(canvasDrawRect)},
    {"canvasDrawRoundRect","(JFFFFFFJJ)V",(void*)JF(canvasDrawRoundRect)},
    {"canvasDrawCircle","(JFFFJJ)V",(void*)JF(canvasDrawCircle)},
    {"canvasDrawLine","(JFFFFJ)V",(void*)JF(canvasDrawLine)},
    {"canvasDrawText","(JLjava/lang/String;FFJJJ)V",(void*)JF(canvasDrawText)},
    {"canvasSave","(J)V",(void*)JF(canvasSave)},{"canvasRestore","(J)V",(void*)JF(canvasRestore)},
    {"canvasTranslate","(JFF)V",(void*)JF(canvasTranslate)},{"canvasScale","(JFF)V",(void*)JF(canvasScale)},
    {"canvasClipRect","(JFFFF)V",(void*)JF(canvasClipRect)},
    {"canvasDrawPath","(JJJJ)V",(void*)JF(canvasDrawPath)},
    {"canvasDrawBitmap","(JJFF)V",(void*)JF(canvasDrawBitmap)},
    {"canvasDrawImage","(J[BFFII)V",(void*)JF(canvasDrawImage)},
    {"canvasConcat","(J[F)V",(void*)JF(canvasConcat)},
    {"canvasRotate","(JFFF)V",(void*)JF(canvasRotate)},
    {"canvasClipPath","(JJ)V",(void*)JF(canvasClipPath)},
    {"canvasDrawArc","(JFFFFFFZJJ)V",(void*)JF(canvasDrawArc)},
    {"canvasDrawOval","(JFFFFJJ)V",(void*)JF(canvasDrawOval)},
    {"penCreate","()J",(void*)JF(penCreate)},{"penSetColor","(JI)V",(void*)JF(penSetColor)},
    {"penSetWidth","(JF)V",(void*)JF(penSetWidth)},{"penSetAntiAlias","(JZ)V",(void*)JF(penSetAntiAlias)},
    {"penSetCap","(JI)V",(void*)JF(penSetCap)},{"penSetJoin","(JI)V",(void*)JF(penSetJoin)},
    {"penDestroy","(J)V",(void*)JF(penDestroy)},
    {"brushCreate","()J",(void*)JF(brushCreate)},{"brushSetColor","(JI)V",(void*)JF(brushSetColor)},
    {"brushDestroy","(J)V",(void*)JF(brushDestroy)},{"brushSetAntiAlias","(JZ)V",(void*)JF(brushSetAntiAlias)},
    {"fontCreate","()J",(void*)JF(fontCreate)},{"fontSetSize","(JF)V",(void*)JF(fontSetSize)},
    {"fontMeasureText","(JLjava/lang/String;)F",(void*)JF(fontMeasureText)},
    {"fontDestroy","(J)V",(void*)JF(fontDestroy)},{"fontGetMetrics","(J)[F",(void*)JF(fontGetMetrics)},
    {"bitmapCreate","(III)J",(void*)JF(bitmapCreate)},{"bitmapDestroy","(J)V",(void*)JF(bitmapDestroy)},
    {"bitmapGetWidth","(J)I",(void*)JF(bitmapGetWidth)},{"bitmapGetHeight","(J)I",(void*)JF(bitmapGetHeight)},
    {"bitmapSetPixel","(JIII)V",(void*)JF(bitmapSetPixel)},{"bitmapGetPixel","(JII)I",(void*)JF(bitmapGetPixel)},
    {"imageDecodeToPixels","([B)[I",(void*)JF(imageDecodeToPixels)},
    {"canvasDrawArgbBitmap","(J[IFFII)V",(void*)JF(canvasDrawArgbBitmap)},
    {"pathCreate","()J",(void*)JF(pathCreate)},{"pathDestroy","(J)V",(void*)JF(pathDestroy)},
    {"pathMoveTo","(JFF)V",(void*)JF(pathMoveTo)},{"pathLineTo","(JFF)V",(void*)JF(pathLineTo)},
    {"pathClose","(J)V",(void*)JF(pathClose)},{"pathReset","(J)V",(void*)JF(pathReset)},
    {"pathQuadTo","(JFFFF)V",(void*)JF(pathQuadTo)},{"pathCubicTo","(JFFFFFF)V",(void*)JF(pathCubicTo)},
    {"pathAddRect","(JFFFFI)V",(void*)JF(pathAddRect)},{"pathAddCircle","(JFFFI)V",(void*)JF(pathAddCircle)},
};

/* ── Framework native method stubs ── */
/* SystemClock + Trace + misc framework natives */
static jlong ohb_sc_elapsedRealtime(JNIEnv* e, jclass c) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static jlong ohb_sc_uptimeMillis(JNIEnv* e, jclass c) { return ohb_sc_elapsedRealtime(e, c); }
static jlong ohb_sc_uptimeNanos(JNIEnv* e, jclass c) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jlong)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static jlong ohb_sc_elapsedRealtimeNanos(JNIEnv* e, jclass c) { return ohb_sc_uptimeNanos(e, c); }
static jlong ohb_sc_currentTimeMicro(JNIEnv* e, jclass c) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (jlong)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static jlong ohb_sc_currentThreadTimeMicro(JNIEnv* e, jclass c) { return ohb_sc_currentTimeMicro(e, c); }
static void ohb_trace_begin(JNIEnv* e, jclass c, jlong tag, jstring name) {}
static void ohb_trace_end(JNIEnv* e, jclass c, jlong tag) {}
static void ohb_trace_asyncBegin(JNIEnv* e, jclass c, jlong tag, jstring name, jint cookie) {}
static void ohb_trace_asyncEnd(JNIEnv* e, jclass c, jlong tag, jstring name, jint cookie) {}
static jboolean ohb_trace_isEnabled(JNIEnv* e, jclass c, jlong tag) { return 0; }
static jlong ohb_trace_nativeGetEnabledTags(JNIEnv* e, jclass c) { return 0; }
/* RuntimeInit */
static void ohb_ri_nativeFinishInit(JNIEnv* e, jclass c) {}
static void ohb_ri_nativeSetExitWithoutCleanup(JNIEnv* e, jclass c, jboolean b) {}
/* ApkAssets — loads compiled resources from APK files */
static int g_apk_count = 0;
static jlong ohb_apk_nativeLoad(JNIEnv* e, jclass c, jint format, jstring path, jint flags, jobject provider) {
    const char* p = path ? (*e)->GetStringUTFChars(e, path, NULL) : "(null)";
    fprintf(stderr, "[ApkAssets] nativeLoad('%s', format=%d, flags=%d)\n", p, format, flags);
    /* Return a fake but non-zero handle. The framework will use this handle
       in nativeGetStringBlock, nativeGetResourceTable, etc. */
    jlong handle = (jlong)(intptr_t)calloc(1, 4096);
    if (path) (*e)->ReleaseStringUTFChars(e, path, p);
    return handle;
}
static jlong ohb_apk_nativeLoadFd(JNIEnv* e, jclass c, jint format, jobject fd, jstring friendlyName, jint flags, jobject provider) {
    return (jlong)(intptr_t)calloc(1, 4096);
}
static jlong ohb_apk_nativeLoadFromPath(JNIEnv* e, jclass c, jstring path, jboolean system) {
    return (jlong)(intptr_t)calloc(1, 4096);
}
static void ohb_apk_nativeDestroy(JNIEnv* e, jclass c, jlong ptr) {}
static jlong ohb_apk_nativeOpenXml(JNIEnv* e, jclass c, jlong ptr, jstring name) { return (jlong)(intptr_t)calloc(1, 4096); }
static jstring ohb_apk_nativeGetAssetPath(JNIEnv* e, jclass c, jlong ptr) { return (*e)->NewStringUTF(e, ""); }
static jlong ohb_apk_nativeGetStringBlock(JNIEnv* e, jclass c, jlong ptr) { return (jlong)(intptr_t)calloc(1, 4096); }
static jboolean ohb_apk_nativeDefinesOverlayable(JNIEnv* e, jclass c, jlong ptr) { return 0; }
static jlong ohb_apk_nativeGetOverlayableInfo(JNIEnv* e, jclass c, jlong ptr, jstring name) { return 0; }
/* XmlBlock natives */
static jlong ohb_xb_nativeGetStringBlock(JNIEnv* e, jclass c, jlong ptr) { return (jlong)(intptr_t)calloc(1, 4096); }
static jlong ohb_xb_nativeCreateParseState(JNIEnv* e, jclass c, jlong ptr, jint resid) { return (jlong)(intptr_t)calloc(1, 4096); }
static jint ohb_xb_nativeNext(JNIEnv* e, jclass c, jlong state) { return 1; /* END_DOCUMENT */ }
static jint ohb_xb_nativeGetNamespace(JNIEnv* e, jclass c, jlong state) { return -1; }
static jint ohb_xb_nativeGetName(JNIEnv* e, jclass c, jlong state) { return -1; }
static jint ohb_xb_nativeGetText(JNIEnv* e, jclass c, jlong state) { return -1; }
static jint ohb_xb_nativeGetAttributeCount(JNIEnv* e, jclass c, jlong state) { return 0; }
static void ohb_xb_nativeDestroyParseState(JNIEnv* e, jclass c, jlong state) {}
static void ohb_xb_nativeDestroy(JNIEnv* e, jclass c, jlong ptr) {}
/* StringBlock natives — needed after ApkAssets.nativeGetStringBlock */
static jint ohb_sb_nativeGetSize(JNIEnv* e, jclass c, jlong ptr) { return 0; }
static jstring ohb_sb_nativeGetString(JNIEnv* e, jclass c, jlong ptr, jint idx) { return (*e)->NewStringUTF(e, ""); }
static jintArray ohb_sb_nativeGetStyle(JNIEnv* e, jclass c, jlong ptr, jint idx) { return NULL; }
static void ohb_sb_nativeDestroy(JNIEnv* e, jclass c, jlong ptr) {}
/* AssetManager native stubs */
static jlong ohb_am_nativeCreate(JNIEnv* e, jclass c) { return (jlong)(intptr_t)calloc(1, 4096); /* real allocated memory */ }
static void ohb_am_nativeDestroy(JNIEnv* e, jclass c, jlong p) {}
static jlong ohb_am_nativeGetThemeFreeFunction(JNIEnv* e, jclass c) { return 0; /* no-op destructor */ }
static void ohb_am_nativeSetApkAssets(JNIEnv* e, jclass c, jlong p, jobjectArray a, jboolean b, jboolean b2) {}
static jlong ohb_am_nativeThemeCreate(JNIEnv* e, jclass c, jlong p) { return (jlong)(intptr_t)calloc(1, 4096); }
static void ohb_am_nativeThemeDestroy(JNIEnv* e, jclass c, jlong p) {}
static void ohb_am_nativeSetConfiguration(JNIEnv* e, jclass c, jlong p,
    jint mcc, jint mnc, jstring locale, jobjectArray localeList,
    jint orientation, jint touchscreen, jint density, jint keyboard,
    jint keyboardHidden, jint navigation, jint screenWidth, jint screenHeight,
    jint smallestScreenWidthDp, jint screenWidthDp, jint screenHeightDp,
    jint screenLayout, jint uiMode, jint colorMode, jint grammaticalGender,
    jboolean forceRefresh) {
    fprintf(stderr, "[AM] nativeSetConfiguration density=%d locale=%s\n", density,
        locale ? "set" : "null");
}
static jint ohb_am_nativeGetResourceValue(JNIEnv* e, jclass c, jlong p, jint id, jshort d, jobject tv, jboolean r) { return 0; }
static jstring ohb_am_nativeGetResourcePackageName(JNIEnv* e, jclass c, jlong p, jint id) { return (*e)->NewStringUTF(e, ""); }
static jstring ohb_am_nativeGetResourceTypeName(JNIEnv* e, jclass c, jlong p, jint id) { return (*e)->NewStringUTF(e, ""); }
static jstring ohb_am_nativeGetResourceEntryName(JNIEnv* e, jclass c, jlong p, jint id) { return (*e)->NewStringUTF(e, ""); }
static jint ohb_am_nativeGetResourceIdentifier(JNIEnv* e, jclass c, jlong p, jstring n, jstring t, jstring pkg) { return 0; }
/* BinderInternal */
static jobject ohb_bi_getContextObject(JNIEnv* e, jclass c) { return NULL; /* no service manager */ }

/* Forward declarations for prop store */
#define MAX_PROPS 256
static struct { char key[128]; char val[512]; } prop_store[MAX_PROPS];
static int prop_count = 0;
static void prop_init_defaults(void);
/* Log.println_native → redirect to stderr */
static jint ohb_log_println(JNIEnv* e, jclass c, jint buf, jint prio, jstring tag, jstring msg) {
    const char* t = tag ? (*e)->GetStringUTFChars(e, tag, NULL) : "?";
    const char* m = msg ? (*e)->GetStringUTFChars(e, msg, NULL) : "";
    fprintf(stderr, "[%s] %s\n", t, m);
    if (tag) (*e)->ReleaseStringUTFChars(e, tag, t);
    if (msg) (*e)->ReleaseStringUTFChars(e, msg, m);
    return 0;
}
static jboolean ohb_log_isLoggable(JNIEnv* e, jclass c, jstring tag, jint level) { return level >= 4; }
static jint ohb_log_maxPayload(JNIEnv* e, jclass c) { return 4068; }
/* Binder stubs */
static jlong ohb_binder_getNativeBBinderHolder(JNIEnv* e, jobject t) { return (jlong)(intptr_t)calloc(1, 256); }
static void ohb_binder_init(JNIEnv* e, jobject t) {}
static jlong ohb_binder_getFinalizer(JNIEnv* e, jclass c) { return 0; }
/* Parcel stubs */
static jlong ohb_parcel_nativeCreate(JNIEnv* e, jclass c) { return (jlong)(intptr_t)calloc(1, 256); }
static void ohb_parcel_nativeDestroy(JNIEnv* e, jclass c, jlong p) {}
static void ohb_parcel_nativeFreeBuffer(JNIEnv* e, jclass c, jlong p) {}
static jint ohb_parcel_nativeDataSize(JNIEnv* e, jclass c, jlong p) { return 0; }
static jint ohb_parcel_nativeDataAvail(JNIEnv* e, jclass c, jlong p) { return 0; }
static jint ohb_parcel_nativeDataPosition(JNIEnv* e, jclass c, jlong p) { return 0; }
/* HardwareRenderer stubs */
static void ohb_hwrender_nSetName(JNIEnv* e, jclass c, jlong p, jstring n) {}
static jlong ohb_hwrender_nCreateProxy(JNIEnv* e, jclass c, jboolean b, jlong p) { return (jlong)(intptr_t)calloc(1, 256); }

static jlong ohb_mq_nativeInit(JNIEnv* e, jobject t) { return (jlong)(intptr_t)calloc(1, 256); }
static void ohb_mq_nativeDestroy(JNIEnv* e, jobject t, jlong p) {}
static void ohb_mq_nativePollOnce(JNIEnv* e, jobject t, jlong p, jint ms) {
    if (ms < 0) ms = 100; if (ms > 1000) ms = 1000; usleep(ms * 1000);
}
static void ohb_mq_nativeWake(JNIEnv* e, jobject t, jlong p) {}
static jboolean ohb_mq_nativeIsPolling(JNIEnv* e, jobject t, jlong p) { return 0; }
static void ohb_mq_nativeSetFdEvents(JNIEnv* e, jobject t, jlong p, jint fd, jint ev) {}
static jstring ohb_sp_get(JNIEnv* e, jclass c, jstring k, jstring d) {
    const char* key = k ? (*e)->GetStringUTFChars(e, k, NULL) : "";
    fprintf(stderr, "[SP-old] get('%s')\n", key);
    jstring result = d;
    /* Return sensible values for Build.* properties */
    /* Use the prop_store for all lookups — it has comprehensive defaults */
    prop_init_defaults();
    for (int i = 0; i < prop_count; i++) {
        if (strcmp(prop_store[i].key, key) == 0) {
            result = (*e)->NewStringUTF(e, prop_store[i].val);
            fprintf(stderr, "[SP-old] → '%s'\n", prop_store[i].val);
            goto sp_done;
        }
    }
sp_done:
    if (!result) result = (*e)->NewStringUTF(e, "");
    if (k) (*e)->ReleaseStringUTFChars(e, k, key);
    return result;
}
static void ohb_sp_set(JNIEnv* e, jclass c, jstring k, jstring v) {}
static jint ohb_sp_get_int(JNIEnv* e, jclass c, jstring k, jint d) { return d; }
static jlong ohb_sp_get_long(JNIEnv* e, jclass c, jstring k, jlong d) { return d; }
static jboolean ohb_sp_get_boolean(JNIEnv* e, jclass c, jstring k, jboolean d) { return d; }

/* Android 15 SystemProperties uses handle-based API:
   native_find(String) -> long handle
   native_get(long handle) -> String value */
/* prop_store/prop_count/MAX_PROPS declared above in forward declarations */

static void prop_init_defaults(void) {
    if (prop_count > 0) return;
    struct { const char* k; const char* v; } defs[] = {
        /* Build identity */
        {"ro.build.id", "RP1A.200720.005"},
        {"ro.build.fingerprint", "westlake/ohos/ohos:11/RP1A.200720.005/1:userdebug/dev-keys"},
        {"ro.build.display.id", "westlake-ohos-userdebug 11 RP1A.200720.005 1 dev-keys"},
        {"ro.build.version.release", "11"}, {"ro.build.version.sdk", "30"},
        {"ro.build.version.incremental", "1"}, {"ro.build.version.codename", "REL"},
        {"ro.build.version.base_os", ""}, {"ro.build.version.security_patch", "2021-01-01"},
        {"ro.build.version.preview_sdk", "0"}, {"ro.build.version.release_or_codename", "11"},
        {"ro.build.version.release_or_preview_display", "11"},
        {"ro.build.version.known_codenames", "Base,Base11,Cupcake,Donut,Eclair,Eclair01,EclairMr1,Froyo,Gingerbread,GingerbreadMr1,Honeycomb,HoneycombMr1,HoneycombMr2,IceCreamSandwich,IceCreamSandwichMr1,JellyBean,JellyBeanMr1,JellyBeanMr2,Kitkat,KitkatWatch,Lollipop,LollipopMr1,M,N,NMr1,O,OMr1,P,Q,R,S,Sv2,Tiramisu,UpsideDownCake,VanillaIceCream"},
        {"ro.build.version.all_codenames", "REL"},
        {"ro.build.version.preview_sdk_fingerprint", "REL"},
        {"ro.odm.build.media_performance_class", "0"},
        {"ro.build.type", "userdebug"}, {"ro.build.tags", "dev-keys"},
        {"ro.build.flavor", "ohos-userdebug"},
        {"ro.build.product", "ohos"},
        {"ro.build.description", "ohos-userdebug 11 RP1A.200720.005 1 dev-keys"},
        {"ro.build.host", "westlake-builder"}, {"ro.build.user", "westlake"},
        {"ro.build.date.utc", "1609459200"},
        /* Product identity */
        {"ro.product.model", "Westlake-OHOS"}, {"ro.product.brand", "westlake"},
        {"ro.product.manufacturer", "Westlake"}, {"ro.product.device", "ohos"},
        {"ro.product.board", "ohos"}, {"ro.product.name", "ohos"},
        {"ro.product.model_for_attestation", ""}, {"ro.product.brand_for_attestation", ""},
        {"ro.product.name_for_attestation", ""}, {"ro.product.device_for_attestation", ""},
        {"ro.product.manufacturer_for_attestation", ""},
        /* CPU / ABI */
        {"ro.product.cpu.abi", "arm64-v8a"},
        {"ro.product.cpu.abilist", "arm64-v8a,armeabi-v7a,armeabi"},
        {"ro.product.cpu.abilist32", "armeabi-v7a,armeabi"},
        {"ro.product.cpu.abilist64", "arm64-v8a"},
        /* Hardware */
        {"ro.hardware", "ohos"}, {"ro.soc.manufacturer", "Westlake"},
        {"ro.soc.model", "OHOS"}, {"ro.bootloader", "unknown"},
        {"ro.boot.hardware.sku", ""}, {"ro.boot.product.hardware.sku", ""},
        {"ro.boot.qemu", "0"},
        /* Radio */
        {"gsm.version.baseband", ""},
        /* System */
        {"ro.debuggable", "1"},
        {"persist.sys.language", "en"}, {"persist.sys.country", "US"},
        {"persist.sys.timezone", "America/New_York"},
        {"persist.sys.dalvik.vm.lib.2", "libart.so"},
    };
    for (int i = 0; i < (int)(sizeof(defs)/sizeof(defs[0])); i++) {
        strncpy(prop_store[i].key, defs[i].k, 127);
        strncpy(prop_store[i].val, defs[i].v, 511);
        prop_count++;
    }
}

static jlong ohb_sp_native_find(JNIEnv* e, jclass c, jstring key) {
    prop_init_defaults();
    if (!key) return 0;
    const char* k = (*e)->GetStringUTFChars(e, key, NULL);
    for (int i = 0; i < prop_count; i++) {
        if (strcmp(prop_store[i].key, k) == 0) {
            fprintf(stderr, "[SP] find(%s) → handle=%d val='%s'\n", k, i+1, prop_store[i].val);
            (*e)->ReleaseStringUTFChars(e, key, k);
            return (jlong)(i + 1); /* 1-based handle */
        }
    }
    /* Not found — add as new empty prop so handle is always valid */
    if (prop_count < MAX_PROPS) {
        strncpy(prop_store[prop_count].key, k, 127);
        prop_store[prop_count].val[0] = '\0';
        prop_count++;
        (*e)->ReleaseStringUTFChars(e, key, k);
        return (jlong)prop_count; /* new handle */
    }
    (*e)->ReleaseStringUTFChars(e, key, k);
    return 0; /* truly out of space */
}

static jstring ohb_sp_native_get_handle(JNIEnv* e, jclass c, jlong handle) {
    prop_init_defaults();
    int idx = (int)(handle - 1);
    if (idx >= 0 && idx < prop_count) {
        fprintf(stderr, "[SP] get(handle=%lld) → '%s' (key=%s)\n", (long long)handle, prop_store[idx].val, prop_store[idx].key);
        return (*e)->NewStringUTF(e, prop_store[idx].val);
    }
    fprintf(stderr, "[SP] get(handle=%lld) → EMPTY (out of range, count=%d)\n", (long long)handle, prop_count);
    return (*e)->NewStringUTF(e, "");
}

static jint ohb_sp_native_get_int_handle(JNIEnv* e, jclass c, jlong h, jint d) {
    prop_init_defaults();
    int idx = (int)(h - 1);
    if (idx >= 0 && idx < prop_count) return atoi(prop_store[idx].val);
    return d;
}

static jlong ohb_sp_native_get_long_handle(JNIEnv* e, jclass c, jlong h, jlong d) {
    prop_init_defaults();
    int idx = (int)(h - 1);
    if (idx >= 0 && idx < prop_count) return atol(prop_store[idx].val);
    return d;
}

static jboolean ohb_sp_native_get_boolean_handle(JNIEnv* e, jclass c, jlong h, jboolean d) {
    prop_init_defaults();
    int idx = (int)(h - 1);
    if (idx >= 0 && idx < prop_count) return strcmp(prop_store[idx].val, "true") == 0 || strcmp(prop_store[idx].val, "1") == 0;
    return d;
}

static void ohb_sp_native_set_handle(JNIEnv* e, jclass c, jstring key, jstring val) {
    prop_init_defaults();
    if (!key) return;
    const char* k = (*e)->GetStringUTFChars(e, key, NULL);
    const char* v = val ? (*e)->GetStringUTFChars(e, val, NULL) : "";
    for (int i = 0; i < prop_count; i++) {
        if (strcmp(prop_store[i].key, k) == 0) {
            strncpy(prop_store[i].val, v, 511);
            goto done;
        }
    }
    if (prop_count < MAX_PROPS) {
        strncpy(prop_store[prop_count].key, k, 127);
        strncpy(prop_store[prop_count].val, v, 511);
        prop_count++;
    }
done:
    (*e)->ReleaseStringUTFChars(e, key, k);
    if (val) (*e)->ReleaseStringUTFChars(e, val, v);
}

/* Try to connect to host app's TCP server for display list IPC.
 * Used when dalvikvm runs from adb shell (SELinux boot image needs shell context).
 * Falls back to stdout pipe for ProcessBuilder mode. */
static int try_tcp_connect(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        fprintf(stderr, "[OHBridge] TCP connected to localhost:%d (fd=%d)\n", port, sock);
        return sock;
    }
    close(sock);
    return -1;
}

static jint OHBridge_JNI_OnLoad_Impl(JavaVM* vm, void* reserved) {
    g_vm = vm;
    sigbus_vm = vm;
    /* Record main thread for signal handler */
    {
        extern pthread_t __ohbridge_main_thread;
        __ohbridge_main_thread = pthread_self();
    }
    /* Install SIGBUS/SIGSEGV handler */
    {
        struct sigaction sa;
        sa.sa_sigaction = sigbus_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);
    }
    /* Use stdout pipe (tcp_pipe handles the TCP forwarding) */
    if (pipe_fd < 0) {
        pipe_fd = dup(STDOUT_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);
    }
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_VERSION_1_6;
    jclass cls = (*env)->FindClass(env, "com/ohos/shim/bridge/OHBridge");
    int ok = 0, count = sizeof(methods)/sizeof(methods[0]);
    if (cls) {
        for (int i = 0; i < count; i++) {
            if ((*env)->RegisterNatives(env, cls, &methods[i], 1) == 0) ok++;
            else (*env)->ExceptionClear(env);
        }
        (*env)->DeleteLocalRef(env, cls);
    } else { (*env)->ExceptionClear(env); }
    fprintf(stderr, "[OHBridge] JNI_OnLoad (pipe stub) %d/%d registered, pipe_fd=%d\n", ok, count, pipe_fd);

    /* ── Framework native stubs (for real framework.jar on BCP) ── */
    {
        /* MessageQueue — core of Android's event loop */
        jclass mqCls = (*env)->FindClass(env, "android/os/MessageQueue");
        if (mqCls) {
            JNINativeMethod mq[] = {
                {"nativeInit", "()J", (void*)ohb_mq_nativeInit},
                {"nativeDestroy", "(J)V", (void*)ohb_mq_nativeDestroy},
                {"nativePollOnce", "(JI)V", (void*)ohb_mq_nativePollOnce},
                {"nativeWake", "(J)V", (void*)ohb_mq_nativeWake},
                {"nativeIsPolling", "(J)Z", (void*)ohb_mq_nativeIsPolling},
                {"nativeSetFileDescriptorEvents", "(JII)V", (void*)ohb_mq_nativeSetFdEvents},
            };
            int i, mq_ok = 0;
            for (i = 0; i < 6; i++) {
                if ((*env)->RegisterNatives(env, mqCls, &mq[i], 1) == 0) mq_ok++;
                else (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] MessageQueue stubs: %d/6\n", mq_ok);
        } else { (*env)->ExceptionClear(env); }

        /* Log */
        jclass logCls = (*env)->FindClass(env, "android/util/Log");
        if (logCls) {
            JNINativeMethod logM[] = {
                {"println_native", "(IILjava/lang/String;Ljava/lang/String;)I", (void*)ohb_log_println},
                {"isLoggable", "(Ljava/lang/String;I)Z", (void*)ohb_log_isLoggable},
                {"logger_entry_max_payload_native", "()I", (void*)ohb_log_maxPayload},
            };
            int i; for (i = 0; i < 3; i++) {
                (*env)->RegisterNatives(env, logCls, &logM[i], 1);
                (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] Log stubs registered\n");
        } else { (*env)->ExceptionClear(env); }

        /* Binder */
        jclass binderCls = (*env)->FindClass(env, "android/os/Binder");
        if (binderCls) {
            JNINativeMethod bm[] = {
                {"getNativeBBinderHolder", "()J", (void*)ohb_binder_getNativeBBinderHolder},
                {"init", "()V", (void*)ohb_binder_init},
                {"getNativeFinalizer", "()J", (void*)ohb_binder_getFinalizer},
            };
            int i; for (i = 0; i < 3; i++) {
                (*env)->RegisterNatives(env, binderCls, &bm[i], 1);
                (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] Binder stubs registered\n");
        } else { (*env)->ExceptionClear(env); }

        /* SystemClock */
        jclass scCls = (*env)->FindClass(env, "android/os/SystemClock");
        if (scCls) {
            JNINativeMethod scM[] = {
                {"elapsedRealtime", "()J", (void*)ohb_sc_elapsedRealtime},
                {"uptimeMillis", "()J", (void*)ohb_sc_uptimeMillis},
                {"uptimeNanos", "()J", (void*)ohb_sc_uptimeNanos},
                {"elapsedRealtimeNanos", "()J", (void*)ohb_sc_elapsedRealtimeNanos},
                {"currentTimeMicro", "()J", (void*)ohb_sc_currentTimeMicro},
                {"currentThreadTimeMicro", "()J", (void*)ohb_sc_currentThreadTimeMicro},
            };
            int i; for (i = 0; i < 4; i++) { (*env)->RegisterNatives(env, scCls, &scM[i], 1); (*env)->ExceptionClear(env); }
            fprintf(stderr, "[OHBridge] SystemClock stubs registered\n");
        } else { (*env)->ExceptionClear(env); }

        /* Trace */
        jclass trCls = (*env)->FindClass(env, "android/os/Trace");
        if (trCls) {
            JNINativeMethod trM[] = {
                {"nativeTraceBegin", "(JLjava/lang/String;)V", (void*)ohb_trace_begin},
                {"nativeTraceEnd", "(J)V", (void*)ohb_trace_end},
                {"nativeAsyncTraceBegin", "(JLjava/lang/String;I)V", (void*)ohb_trace_asyncBegin},
                {"nativeAsyncTraceEnd", "(JLjava/lang/String;I)V", (void*)ohb_trace_asyncEnd},
                {"nativeIsTagEnabled", "(J)Z", (void*)ohb_trace_isEnabled},
                {"nativeGetEnabledTags", "()J", (void*)ohb_trace_nativeGetEnabledTags},
            };
            int i; for (i = 0; i < 6; i++) { (*env)->RegisterNatives(env, trCls, &trM[i], 1); (*env)->ExceptionClear(env); }
            fprintf(stderr, "[OHBridge] Trace stubs registered\n");
        } else { (*env)->ExceptionClear(env); }

        /* ApkAssets */
        jclass aaCls = (*env)->FindClass(env, "android/content/res/ApkAssets");
        if (aaCls) {
            JNINativeMethod aaM[] = {
                {"nativeLoad", "(ILjava/lang/String;ILandroid/content/res/loader/AssetsProvider;)J", (void*)ohb_apk_nativeLoad},
                {"nativeLoadEmpty", "(ILandroid/content/res/loader/AssetsProvider;)J", (void*)ohb_apk_nativeLoadFd},
                {"nativeDestroy", "(J)V", (void*)ohb_apk_nativeDestroy},
                {"nativeGetAssetPath", "(J)Ljava/lang/String;", (void*)ohb_apk_nativeGetAssetPath},
                {"nativeGetStringBlock", "(J)J", (void*)ohb_apk_nativeGetStringBlock},
                {"nativeDefinesOverlayable", "(J)Z", (void*)ohb_apk_nativeDefinesOverlayable},
                {"nativeOpenXml", "(JLjava/lang/String;)J", (void*)ohb_apk_nativeOpenXml},
            };
            int i, aa_ok = 0;
            for (i = 0; i < 7; i++) {
                if ((*env)->RegisterNatives(env, aaCls, &aaM[i], 1) == 0) aa_ok++;
                else (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] ApkAssets stubs: %d/7\n", aa_ok);
        } else { (*env)->ExceptionClear(env); }

        /* XmlBlock */
        jclass xbCls = (*env)->FindClass(env, "android/content/res/XmlBlock");
        if (xbCls) {
            JNINativeMethod xbM[] = {
                {"nativeGetStringBlock", "(J)J", (void*)ohb_xb_nativeGetStringBlock},
                {"nativeCreateParseState", "(JI)J", (void*)ohb_xb_nativeCreateParseState},
                {"nativeNext", "(J)I", (void*)ohb_xb_nativeNext},
                {"nativeGetNamespace", "(J)I", (void*)ohb_xb_nativeGetNamespace},
                {"nativeGetName", "(J)I", (void*)ohb_xb_nativeGetName},
                {"nativeGetText", "(J)I", (void*)ohb_xb_nativeGetText},
                {"nativeGetAttributeCount", "(J)I", (void*)ohb_xb_nativeGetAttributeCount},
                {"nativeDestroyParseState", "(J)V", (void*)ohb_xb_nativeDestroyParseState},
                {"nativeDestroy", "(J)V", (void*)ohb_xb_nativeDestroy},
            };
            int i, xb_ok = 0;
            for (i = 0; i < 9; i++) {
                if ((*env)->RegisterNatives(env, xbCls, &xbM[i], 1) == 0) xb_ok++;
                else (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] XmlBlock stubs: %d/9\n", xb_ok);
        } else { (*env)->ExceptionClear(env); }

        /* StringBlock */
        jclass sbCls = (*env)->FindClass(env, "android/content/res/StringBlock");
        if (sbCls) {
            JNINativeMethod sbM[] = {
                {"nativeGetSize", "(J)I", (void*)ohb_sb_nativeGetSize},
                {"nativeGetString", "(JI)Ljava/lang/String;", (void*)ohb_sb_nativeGetString},
                {"nativeGetStyle", "(JI)[I", (void*)ohb_sb_nativeGetStyle},
                {"nativeDestroy", "(J)V", (void*)ohb_sb_nativeDestroy},
            };
            int i, sb_ok = 0;
            for (i = 0; i < 4; i++) {
                if ((*env)->RegisterNatives(env, sbCls, &sbM[i], 1) == 0) sb_ok++;
                else (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] StringBlock stubs: %d/4\n", sb_ok);
        } else { (*env)->ExceptionClear(env); }

        /* AssetManager */
        jclass amCls = (*env)->FindClass(env, "android/content/res/AssetManager");
        if (amCls) {
            JNINativeMethod amM[] = {
                {"nativeCreate", "()J", (void*)ohb_am_nativeCreate},
                {"nativeDestroy", "(J)V", (void*)ohb_am_nativeDestroy},
                {"nativeGetThemeFreeFunction", "()J", (void*)ohb_am_nativeGetThemeFreeFunction},
                {"nativeSetApkAssets", "(J[Landroid/content/res/ApkAssets;ZZ)V", (void*)ohb_am_nativeSetApkAssets},
                {"nativeThemeCreate", "(J)J", (void*)ohb_am_nativeThemeCreate},
                {"nativeThemeDestroy", "(J)V", (void*)ohb_am_nativeThemeDestroy},
                {"nativeSetConfiguration", "(JIILjava/lang/String;[Ljava/lang/String;IIIIIIIIIIIIIIIIZ)V", (void*)ohb_am_nativeSetConfiguration},
                {"nativeGetResourceValue", "(JISLandroid/util/TypedValue;Z)I", (void*)ohb_am_nativeGetResourceValue},
                {"nativeGetResourcePackageName", "(JI)Ljava/lang/String;", (void*)ohb_am_nativeGetResourcePackageName},
                {"nativeGetResourceTypeName", "(JI)Ljava/lang/String;", (void*)ohb_am_nativeGetResourceTypeName},
                {"nativeGetResourceEntryName", "(JI)Ljava/lang/String;", (void*)ohb_am_nativeGetResourceEntryName},
                {"nativeGetResourceIdentifier", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", (void*)ohb_am_nativeGetResourceIdentifier},
            };
            int i, am_ok = 0;
            for (i = 0; i < 11; i++) {
                if ((*env)->RegisterNatives(env, amCls, &amM[i], 1) == 0) am_ok++;
                else (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] AssetManager stubs: %d/11\n", am_ok);
        } else { (*env)->ExceptionClear(env); }

        /* BinderInternal */
        jclass biCls = (*env)->FindClass(env, "com/android/internal/os/BinderInternal");
        if (biCls) {
            JNINativeMethod biM[] = {
                {"getContextObject", "()Landroid/os/IBinder;", (void*)ohb_bi_getContextObject},
            };
            (*env)->RegisterNatives(env, biCls, biM, 1);
            (*env)->ExceptionClear(env);
            fprintf(stderr, "[OHBridge] BinderInternal stubs registered\n");
        } else { (*env)->ExceptionClear(env); }

        /* SystemProperties — both old (String-based) and new (handle-based) APIs */
        jclass spCls = (*env)->FindClass(env, "android/os/SystemProperties");
        if (spCls) {
            JNINativeMethod sp[] = {
                /* Old API (Android ≤14) */
                {"native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", (void*)ohb_sp_get},
                {"native_set", "(Ljava/lang/String;Ljava/lang/String;)V", (void*)ohb_sp_set},
                {"native_get_int", "(Ljava/lang/String;I)I", (void*)ohb_sp_get_int},
                {"native_get_long", "(Ljava/lang/String;J)J", (void*)ohb_sp_get_long},
                {"native_get_boolean", "(Ljava/lang/String;Z)Z", (void*)ohb_sp_get_boolean},
                /* New API (Android 15+) — handle-based */
                {"native_find", "(Ljava/lang/String;)J", (void*)ohb_sp_native_find},
                {"native_get", "(J)Ljava/lang/String;", (void*)ohb_sp_native_get_handle},
                {"native_get_int", "(JI)I", (void*)ohb_sp_native_get_int_handle},
                {"native_get_long", "(JJ)J", (void*)ohb_sp_native_get_long_handle},
                {"native_get_boolean", "(JZ)Z", (void*)ohb_sp_native_get_boolean_handle},
                {"native_set", "(Ljava/lang/String;Ljava/lang/String;)V", (void*)ohb_sp_native_set_handle},
            };
            int i, sp_ok = 0;
            for (i = 0; i < 11; i++) {
                if ((*env)->RegisterNatives(env, spCls, &sp[i], 1) == 0) sp_ok++;
                else (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] SystemProperties stubs: %d/11\n", sp_ok);
        } else { (*env)->ExceptionClear(env); }
    }

    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    return OHBridge_JNI_OnLoad_Impl(vm, reserved);
}

JNIEXPORT jint JNI_OnLoad_ohbridge_real(JavaVM* vm, void* reserved) {
    return OHBridge_JNI_OnLoad_Impl(vm, reserved);
}

/* MessageQueue native stubs for Looper/Handler support */
#include <sys/epoll.h>
#include <sys/eventfd.h>

static jlong MessageQueue_nativeInit(JNIEnv* env, jobject obj) {
    // Create epoll fd + event fd (minimal Looper implementation)
    int epollFd = epoll_create1(EPOLL_CLOEXEC);
    int eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (epollFd >= 0 && eventFd >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = eventFd;
        epoll_ctl(epollFd, EPOLL_CTL_ADD, eventFd, &ev);
    }
    // Return epollFd as the "native pointer" (Looper stores this)
    return (jlong)((((long long)epollFd) << 32) | (eventFd & 0xFFFFFFFFL));
}
static void MessageQueue_nativeDestroy(JNIEnv* env, jobject obj, jlong ptr) {
    int epollFd = (int)(ptr >> 32);
    int eventFd = (int)(ptr & 0xFFFFFFFFL);
    if (epollFd >= 0) close(epollFd);
    if (eventFd >= 0) close(eventFd);
}
static void MessageQueue_nativePollOnce(JNIEnv* env, jobject obj, jlong ptr, jint timeoutMillis) {
    int epollFd = (int)(ptr >> 32);
    struct epoll_event events[8];
    epoll_wait(epollFd, events, 8, timeoutMillis);
}
static void MessageQueue_nativeWake(JNIEnv* env, jobject obj, jlong ptr) {
    int eventFd = (int)(ptr & 0xFFFFFFFFL);
    uint64_t val = 1;
    write(eventFd, &val, sizeof(val));
}
static jboolean MessageQueue_nativeIsPolling(JNIEnv* env, jobject obj, jlong ptr) {
    return JNI_FALSE;
}
static void MessageQueue_nativeSetFileDescriptorEvents(JNIEnv* env, jobject obj,
    jlong ptr, jint fd, jint events) {
    // No-op for now
}
