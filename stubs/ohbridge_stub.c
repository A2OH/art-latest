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

enum { OP_COLOR=1,OP_RECT=2,OP_TEXT=3,OP_LINE=4,OP_SAVE=5,OP_RESTORE=6,OP_TRANSLATE=7,OP_CLIP=8,OP_RRECT=9,OP_CIRCLE=10,OP_IMAGE=11,OP_ARGB_BITMAP=12,OP_PATH=13 };

#define MAX_H 256
static int h_colors[MAX_H];
static float h_fontsz[MAX_H];
static int h_next = 1;
static int idx(long h) { return (int)(h & 0xFF); }

/* ── Path storage ── */
/* Stored as a parallel binary command stream. Cap at 4KB per path. */
#define PATH_BUF_MAX  4096
#define PATH_POOL_MAX 8
typedef struct {
    int  in_use;
    int  pos;      /* bytes used */
    int  cmd_count;
    unsigned char buf[PATH_BUF_MAX];
} OhPath;
static OhPath g_paths[PATH_POOL_MAX];

static OhPath* path_get(jlong h) {
    int i = (int)(h - 1);
    if (i < 0 || i >= PATH_POOL_MAX) return NULL;
    if (!g_paths[i].in_use) return NULL;
    return &g_paths[i];
}
static void path_emit1(OhPath* p, unsigned char v) {
    if (p->pos + 1 > PATH_BUF_MAX) return;
    p->buf[p->pos++] = v;
}
static void path_emitf(OhPath* p, float v) {
    if (p->pos + 4 > PATH_BUF_MAX) return;
    memcpy(p->buf + p->pos, &v, 4);
    p->pos += 4;
}
enum { PCMD_MOVE=0, PCMD_LINE=1, PCMD_QUAD=2, PCMD_CUBIC=3, PCMD_CLOSE=4 };

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
JNIEXPORT jlong JNICALL JF(surfaceCreate)(JNIEnv* e, jclass c, jlong u, jint w, jint h) {
    int pending = (*e)->ExceptionCheck(e) ? 1 : 0;
    fprintf(stderr,
            "[PF202N] ohbridge_stub surfaceCreate entry handle=%lld w=%d h=%d pending=%d pipe_fd=%d\n",
            (long long)u,
            (int)w,
            (int)h,
            pending,
            pipe_fd);
    fflush(stderr);
    fprintf(stderr, "[PF202N] ohbridge_stub surfaceCreate return=1\n");
    fflush(stderr);
    return 1;
}
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
            if (op == OP_PATH && i + 7 <= size) {
                /* [op(1)][color(4i)][payloadLen(2i)][payload(payloadLen)] */
                short plen = *(short*)(dlist_buf + i + 5);
                if (plen >= 0 && plen <= size - i - 7) {
                    i += 7 + plen;
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
JNIEXPORT void JNICALL JF(canvasDrawPath)(JNIEnv* e, jclass c, jlong cn, jlong path, jlong pen, jlong brush) {
    OhPath* pp = path_get(path);
    if (!pp || pp->pos <= 0) return;
    /* Brush wins over pen for fill color (matches canvasDrawRect convention). */
    int color = h_colors[idx(brush > 0 ? brush : pen)];
    int header = 1 + 4 + 2; /* op + color + payloadLen */
    if (pp->pos > 65535) return; /* won't fit in short */
    if (dlist_pos + header + pp->pos > DLIST_MAX - 64) return;
    emit1(OP_PATH);
    emiti(color);
    emit2((short)pp->pos);
    memcpy(dlist_buf + dlist_pos, pp->buf, pp->pos);
    dlist_pos += pp->pos;
}
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

JNIEXPORT jlong JNICALL JF(pathCreate)(JNIEnv* e, jclass c) {
    for (int i = 0; i < PATH_POOL_MAX; i++) {
        if (!g_paths[i].in_use) {
            g_paths[i].in_use = 1;
            g_paths[i].pos = 0;
            g_paths[i].cmd_count = 0;
            return (jlong)(i + 1);
        }
    }
    /* Pool exhausted — recycle slot 0 */
    g_paths[0].in_use = 1;
    g_paths[0].pos = 0;
    g_paths[0].cmd_count = 0;
    return 1;
}
JNIEXPORT void JNICALL JF(pathDestroy)(JNIEnv* e, jclass c, jlong p) {
    OhPath* pp = path_get(p);
    if (pp) { pp->in_use = 0; pp->pos = 0; pp->cmd_count = 0; }
}
JNIEXPORT void JNICALL JF(pathMoveTo)(JNIEnv* e, jclass c, jlong p, jfloat x, jfloat y) {
    OhPath* pp = path_get(p); if (!pp) return;
    if (pp->pos + 1 + 8 > PATH_BUF_MAX) return;
    path_emit1(pp, PCMD_MOVE); path_emitf(pp, x); path_emitf(pp, y);
    pp->cmd_count++;
}
JNIEXPORT void JNICALL JF(pathLineTo)(JNIEnv* e, jclass c, jlong p, jfloat x, jfloat y) {
    OhPath* pp = path_get(p); if (!pp) return;
    if (pp->pos + 1 + 8 > PATH_BUF_MAX) return;
    path_emit1(pp, PCMD_LINE); path_emitf(pp, x); path_emitf(pp, y);
    pp->cmd_count++;
}
JNIEXPORT void JNICALL JF(pathClose)(JNIEnv* e, jclass c, jlong p) {
    OhPath* pp = path_get(p); if (!pp) return;
    if (pp->pos + 1 > PATH_BUF_MAX) return;
    path_emit1(pp, PCMD_CLOSE);
    pp->cmd_count++;
}
JNIEXPORT void JNICALL JF(pathReset)(JNIEnv* e, jclass c, jlong p) {
    OhPath* pp = path_get(p); if (!pp) return;
    pp->pos = 0; pp->cmd_count = 0;
}
JNIEXPORT void JNICALL JF(pathQuadTo)(JNIEnv* e, jclass c, jlong p, jfloat x1, jfloat y1, jfloat x2, jfloat y2) {
    OhPath* pp = path_get(p); if (!pp) return;
    if (pp->pos + 1 + 16 > PATH_BUF_MAX) return;
    path_emit1(pp, PCMD_QUAD);
    path_emitf(pp, x1); path_emitf(pp, y1);
    path_emitf(pp, x2); path_emitf(pp, y2);
    pp->cmd_count++;
}
JNIEXPORT void JNICALL JF(pathCubicTo)(JNIEnv* e, jclass c, jlong p, jfloat x1, jfloat y1, jfloat x2, jfloat y2, jfloat x3, jfloat y3) {
    OhPath* pp = path_get(p); if (!pp) return;
    if (pp->pos + 1 + 24 > PATH_BUF_MAX) return;
    path_emit1(pp, PCMD_CUBIC);
    path_emitf(pp, x1); path_emitf(pp, y1);
    path_emitf(pp, x2); path_emitf(pp, y2);
    path_emitf(pp, x3); path_emitf(pp, y3);
    pp->cmd_count++;
}
JNIEXPORT void JNICALL JF(pathAddRect)(JNIEnv* e, jclass c, jlong p, jfloat l, jfloat t, jfloat r, jfloat b, jint dir) {
    OhPath* pp = path_get(p); if (!pp) return;
    /* Emit M l,t L r,t L r,b L l,b Z (CW). Reverse if dir != 0 (CCW). */
    if (dir == 0) {
        JF(pathMoveTo)(e, c, p, l, t);
        JF(pathLineTo)(e, c, p, r, t);
        JF(pathLineTo)(e, c, p, r, b);
        JF(pathLineTo)(e, c, p, l, b);
    } else {
        JF(pathMoveTo)(e, c, p, l, t);
        JF(pathLineTo)(e, c, p, l, b);
        JF(pathLineTo)(e, c, p, r, b);
        JF(pathLineTo)(e, c, p, r, t);
    }
    JF(pathClose)(e, c, p);
}
JNIEXPORT void JNICALL JF(pathAddCircle)(JNIEnv* e, jclass c, jlong p, jfloat cx, jfloat cy, jfloat r, jint dir) {
    OhPath* pp = path_get(p); if (!pp) return;
    /* Approximate circle with 4 cubic Beziers (kappa ~ 0.5522847498). */
    const float K = 0.5522847498f * r;
    JF(pathMoveTo)(e, c, p, cx + r, cy);
    if (dir == 0) {
        JF(pathCubicTo)(e, c, p, cx + r, cy + K, cx + K, cy + r, cx, cy + r);
        JF(pathCubicTo)(e, c, p, cx - K, cy + r, cx - r, cy + K, cx - r, cy);
        JF(pathCubicTo)(e, c, p, cx - r, cy - K, cx - K, cy - r, cx, cy - r);
        JF(pathCubicTo)(e, c, p, cx + K, cy - r, cx + r, cy - K, cx + r, cy);
    } else {
        JF(pathCubicTo)(e, c, p, cx + r, cy - K, cx + K, cy - r, cx, cy - r);
        JF(pathCubicTo)(e, c, p, cx - K, cy - r, cx - r, cy - K, cx - r, cy);
        JF(pathCubicTo)(e, c, p, cx - r, cy + K, cx - K, cy + r, cx, cy + r);
        JF(pathCubicTo)(e, c, p, cx + K, cy + r, cx + r, cy + K, cx + r, cy);
    }
    JF(pathClose)(e, c, p);
}

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
JNIEXPORT jstring JNICALL JF(telephonyGetDeviceId)(JNIEnv* e, jclass c) { return (*e)->NewStringUTF(e,""); }
JNIEXPORT jstring JNICALL JF(telephonyGetLine1Number)(JNIEnv* e, jclass c) { return (*e)->NewStringUTF(e,""); }
JNIEXPORT jstring JNICALL JF(telephonyGetNetworkOperatorName)(JNIEnv* e, jclass c) { return (*e)->NewStringUTF(e,"Westlake"); }
JNIEXPORT jint JNICALL JF(telephonyGetSimState)(JNIEnv* e, jclass c) { return 5; /* SIM_STATE_READY */ }
JNIEXPORT jint JNICALL JF(telephonyGetPhoneType)(JNIEnv* e, jclass c) { return 1; /* PHONE_TYPE_GSM */ }
JNIEXPORT jint JNICALL JF(telephonyGetNetworkType)(JNIEnv* e, jclass c) { return 13; /* NETWORK_TYPE_LTE */ }

/* === Safe southbound defaults ===
 * These methods close the gap between the Java OHBridge contract and the
 * standalone static ART registration table. They are intentionally conservative
 * compatibility defaults, not complete service implementations.
 */
static jlong ohb_next_stub_handle = 1000;
static char ohb_clipboard[4096] = {0};

static jstring ohb_empty_string(JNIEnv* e) { return (*e)->NewStringUTF(e, ""); }
static jstring ohb_json_empty(JNIEnv* e) { return (*e)->NewStringUTF(e, "{}"); }
static jbyteArray ohb_empty_bytes(JNIEnv* e) { return (*e)->NewByteArray(e, 0); }
static jdoubleArray ohb_location_array(JNIEnv* e) {
    jdoubleArray a = (*e)->NewDoubleArray(e, 2);
    if (a) {
        jdouble v[2] = {37.7749, -122.4194};
        (*e)->SetDoubleArrayRegion(e, a, 0, 2, v);
    }
    return a;
}
static jfloatArray ohb_sensor_array(JNIEnv* e) {
    jfloatArray a = (*e)->NewFloatArray(e, 3);
    if (a) {
        jfloat v[3] = {0.0f, 0.0f, 0.0f};
        (*e)->SetFloatArrayRegion(e, a, 0, 3, v);
    }
    return a;
}

JNIEXPORT jlong JNICALL JF(preferencesOpen)(JNIEnv* e, jclass c, jstring name) { return ohb_next_stub_handle++; }
JNIEXPORT jstring JNICALL JF(preferencesGetString)(JNIEnv* e, jclass c, jlong h, jstring k, jstring d) { return d ? d : ohb_empty_string(e); }
JNIEXPORT jint JNICALL JF(preferencesGetInt)(JNIEnv* e, jclass c, jlong h, jstring k, jint d) { return d; }
JNIEXPORT jlong JNICALL JF(preferencesGetLong)(JNIEnv* e, jclass c, jlong h, jstring k, jlong d) { return d; }
JNIEXPORT jfloat JNICALL JF(preferencesGetFloat)(JNIEnv* e, jclass c, jlong h, jstring k, jfloat d) { return d; }
JNIEXPORT jboolean JNICALL JF(preferencesGetBoolean)(JNIEnv* e, jclass c, jlong h, jstring k, jboolean d) { return d; }
JNIEXPORT void JNICALL JF(preferencesPutString)(JNIEnv* e, jclass c, jlong h, jstring k, jstring v) {}
JNIEXPORT void JNICALL JF(preferencesPutInt)(JNIEnv* e, jclass c, jlong h, jstring k, jint v) {}
JNIEXPORT void JNICALL JF(preferencesPutLong)(JNIEnv* e, jclass c, jlong h, jstring k, jlong v) {}
JNIEXPORT void JNICALL JF(preferencesPutFloat)(JNIEnv* e, jclass c, jlong h, jstring k, jfloat v) {}
JNIEXPORT void JNICALL JF(preferencesPutBoolean)(JNIEnv* e, jclass c, jlong h, jstring k, jboolean v) {}
JNIEXPORT void JNICALL JF(preferencesFlush)(JNIEnv* e, jclass c, jlong h) {}
JNIEXPORT void JNICALL JF(preferencesRemove)(JNIEnv* e, jclass c, jlong h, jstring k) {}
JNIEXPORT void JNICALL JF(preferencesClear)(JNIEnv* e, jclass c, jlong h) {}
JNIEXPORT void JNICALL JF(preferencesClose)(JNIEnv* e, jclass c, jlong h) {}

JNIEXPORT jlong JNICALL JF(rdbStoreOpen)(JNIEnv* e, jclass c, jstring db, jint version) { return ohb_next_stub_handle++; }
JNIEXPORT void JNICALL JF(rdbStoreExecSQL)(JNIEnv* e, jclass c, jlong h, jstring sql) {}
JNIEXPORT jlong JNICALL JF(rdbStoreQuery)(JNIEnv* e, jclass c, jlong h, jstring sql, jobjectArray args) { return ohb_next_stub_handle++; }
JNIEXPORT jlong JNICALL JF(rdbStoreInsert)(JNIEnv* e, jclass c, jlong h, jstring table, jstring valuesJson) { return 1; }
JNIEXPORT jint JNICALL JF(rdbStoreUpdate)(JNIEnv* e, jclass c, jlong h, jstring valuesJson, jstring table, jstring whereClause, jobjectArray whereArgs) { return 0; }
JNIEXPORT jint JNICALL JF(rdbStoreDelete)(JNIEnv* e, jclass c, jlong h, jstring table, jstring whereClause, jobjectArray whereArgs) { return 0; }
JNIEXPORT void JNICALL JF(rdbStoreBeginTransaction)(JNIEnv* e, jclass c, jlong h) {}
JNIEXPORT void JNICALL JF(rdbStoreCommit)(JNIEnv* e, jclass c, jlong h) {}
JNIEXPORT void JNICALL JF(rdbStoreRollback)(JNIEnv* e, jclass c, jlong h) {}
JNIEXPORT void JNICALL JF(rdbStoreClose)(JNIEnv* e, jclass c, jlong h) {}

JNIEXPORT jboolean JNICALL JF(resultSetGoToFirstRow)(JNIEnv* e, jclass c, jlong h) { return JNI_FALSE; }
JNIEXPORT jboolean JNICALL JF(resultSetGoToNextRow)(JNIEnv* e, jclass c, jlong h) { return JNI_FALSE; }
JNIEXPORT jint JNICALL JF(resultSetGetColumnIndex)(JNIEnv* e, jclass c, jlong h, jstring name) { return -1; }
JNIEXPORT jstring JNICALL JF(resultSetGetString)(JNIEnv* e, jclass c, jlong h, jint index) { return ohb_empty_string(e); }
JNIEXPORT jint JNICALL JF(resultSetGetInt)(JNIEnv* e, jclass c, jlong h, jint index) { return 0; }
JNIEXPORT jlong JNICALL JF(resultSetGetLong)(JNIEnv* e, jclass c, jlong h, jint index) { return 0; }
JNIEXPORT jfloat JNICALL JF(resultSetGetFloat)(JNIEnv* e, jclass c, jlong h, jint index) { return 0.0f; }
JNIEXPORT jdouble JNICALL JF(resultSetGetDouble)(JNIEnv* e, jclass c, jlong h, jint index) { return 0.0; }
JNIEXPORT jbyteArray JNICALL JF(resultSetGetBlob)(JNIEnv* e, jclass c, jlong h, jint index) { return ohb_empty_bytes(e); }
JNIEXPORT jboolean JNICALL JF(resultSetIsNull)(JNIEnv* e, jclass c, jlong h, jint index) { return JNI_TRUE; }
JNIEXPORT jint JNICALL JF(resultSetGetRowCount)(JNIEnv* e, jclass c, jlong h) { return 0; }
JNIEXPORT jint JNICALL JF(resultSetGetColumnCount)(JNIEnv* e, jclass c, jlong h) { return 0; }
JNIEXPORT jstring JNICALL JF(resultSetGetColumnName)(JNIEnv* e, jclass c, jlong h, jint index) { return ohb_empty_string(e); }
JNIEXPORT void JNICALL JF(resultSetClose)(JNIEnv* e, jclass c, jlong h) {}

JNIEXPORT void JNICALL JF(notificationPublish)(JNIEnv* e, jclass c, jint id, jstring title, jstring text, jstring channelId, jint priority) {}
JNIEXPORT void JNICALL JF(notificationCancel)(JNIEnv* e, jclass c, jint id) {}
JNIEXPORT void JNICALL JF(notificationAddSlot)(JNIEnv* e, jclass c, jstring channelId, jstring channelName, jint importance) {}
JNIEXPORT jint JNICALL JF(reminderScheduleTimer)(JNIEnv* e, jclass c, jint delaySeconds, jstring title, jstring content, jstring targetAbility, jstring paramsJson) { return 0; }
JNIEXPORT void JNICALL JF(reminderCancel)(JNIEnv* e, jclass c, jint id) {}
JNIEXPORT void JNICALL JF(startAbility)(JNIEnv* e, jclass c, jstring bundle, jstring ability, jstring paramsJson) {}
JNIEXPORT void JNICALL JF(terminateSelf)(JNIEnv* e, jclass c) {}

JNIEXPORT void JNICALL JF(logWarn)(JNIEnv* e, jclass c, jstring tag, jstring msg) {
    if(!tag||!msg) return;
    const char* t=(*e)->GetStringUTFChars(e,tag,0);
    const char* m=(*e)->GetStringUTFChars(e,msg,0);
    fprintf(stderr,"W/%s: %s\n",t?t:"?",m?m:"");
    if(t)(*e)->ReleaseStringUTFChars(e,tag,t);
    if(m)(*e)->ReleaseStringUTFChars(e,msg,m);
}
JNIEXPORT void JNICALL JF(showToast)(JNIEnv* e, jclass c, jstring message, jint duration) {}
JNIEXPORT jstring JNICALL JF(httpRequest)(JNIEnv* e, jclass c, jstring url, jstring method, jstring headersJson, jstring body) { return ohb_json_empty(e); }
JNIEXPORT jboolean JNICALL JF(isNetworkAvailable)(JNIEnv* e, jclass c) { return JNI_TRUE; }
JNIEXPORT jint JNICALL JF(getNetworkType)(JNIEnv* e, jclass c) { return 1; /* Wi-Fi */ }
JNIEXPORT jboolean JNICALL JF(wifiIsEnabled)(JNIEnv* e, jclass c) { return JNI_TRUE; }
JNIEXPORT jboolean JNICALL JF(wifiSetEnabled)(JNIEnv* e, jclass c, jboolean enabled) { return enabled; }
JNIEXPORT jint JNICALL JF(wifiGetState)(JNIEnv* e, jclass c) { return 3; /* WIFI_STATE_ENABLED */ }
JNIEXPORT jstring JNICALL JF(wifiGetSSID)(JNIEnv* e, jclass c) { return (*e)->NewStringUTF(e, "Westlake"); }
JNIEXPORT jint JNICALL JF(wifiGetRssi)(JNIEnv* e, jclass c) { return -50; }
JNIEXPORT jint JNICALL JF(wifiGetLinkSpeed)(JNIEnv* e, jclass c) { return 866; }
JNIEXPORT jint JNICALL JF(wifiGetFrequency)(JNIEnv* e, jclass c) { return 5200; }
JNIEXPORT jdoubleArray JNICALL JF(locationGetLast)(JNIEnv* e, jclass c) { return ohb_location_array(e); }
JNIEXPORT jboolean JNICALL JF(locationIsEnabled)(JNIEnv* e, jclass c) { return JNI_TRUE; }

JNIEXPORT jlong JNICALL JF(nodeCreate)(JNIEnv* e, jclass c, jint nodeType) { return ohb_next_stub_handle++; }
JNIEXPORT void JNICALL JF(nodeDispose)(JNIEnv* e, jclass c, jlong node) {}
JNIEXPORT void JNICALL JF(nodeAddChild)(JNIEnv* e, jclass c, jlong parent, jlong child) {}
JNIEXPORT void JNICALL JF(nodeRemoveChild)(JNIEnv* e, jclass c, jlong parent, jlong child) {}
JNIEXPORT void JNICALL JF(nodeInsertChildAt)(JNIEnv* e, jclass c, jlong parent, jlong child, jint position) {}
JNIEXPORT jint JNICALL JF(nodeSetAttrFloat)(JNIEnv* e, jclass c, jlong node, jint attrType, jfloat v0, jfloat v1, jfloat v2, jfloat v3, jint count) { return 0; }
JNIEXPORT jint JNICALL JF(nodeSetAttrColor)(JNIEnv* e, jclass c, jlong node, jint attrType, jint color) { return 0; }
JNIEXPORT jint JNICALL JF(nodeSetAttrInt)(JNIEnv* e, jclass c, jlong node, jint attrType, jint value) { return 0; }
JNIEXPORT jint JNICALL JF(nodeSetAttrString)(JNIEnv* e, jclass c, jlong node, jint attrType, jstring value) { return 0; }
JNIEXPORT jint JNICALL JF(nodeRegisterEvent)(JNIEnv* e, jclass c, jlong node, jint eventType, jint eventId) { return 0; }
JNIEXPORT void JNICALL JF(nodeUnregisterEvent)(JNIEnv* e, jclass c, jlong node, jint eventType) {}
JNIEXPORT void JNICALL JF(nodeMarkDirty)(JNIEnv* e, jclass c, jlong node, jint flag) {}

JNIEXPORT void JNICALL JF(clipboardSet)(JNIEnv* e, jclass c, jstring text) {
    const char* s = text ? (*e)->GetStringUTFChars(e, text, 0) : "";
    snprintf(ohb_clipboard, sizeof(ohb_clipboard), "%s", s ? s : "");
    if (text && s) (*e)->ReleaseStringUTFChars(e, text, s);
}
JNIEXPORT jstring JNICALL JF(clipboardGet)(JNIEnv* e, jclass c) { return (*e)->NewStringUTF(e, ohb_clipboard); }

JNIEXPORT jint JNICALL JF(audioGetStreamVolume)(JNIEnv* e, jclass c, jint streamType) { return 8; }
JNIEXPORT jint JNICALL JF(audioGetStreamMaxVolume)(JNIEnv* e, jclass c, jint streamType) { return 15; }
JNIEXPORT void JNICALL JF(audioSetStreamVolume)(JNIEnv* e, jclass c, jint streamType, jint index, jint flags) {}
JNIEXPORT jint JNICALL JF(audioGetRingerMode)(JNIEnv* e, jclass c) { return 2; /* RINGER_MODE_NORMAL */ }
JNIEXPORT void JNICALL JF(audioSetRingerMode)(JNIEnv* e, jclass c, jint mode) {}
JNIEXPORT jboolean JNICALL JF(audioIsMusicActive)(JNIEnv* e, jclass c) { return JNI_FALSE; }

JNIEXPORT jlong JNICALL JF(mediaPlayerCreate)(JNIEnv* e, jclass c) { return ohb_next_stub_handle++; }
JNIEXPORT void JNICALL JF(mediaPlayerSetDataSource)(JNIEnv* e, jclass c, jlong handle, jstring path) {}
JNIEXPORT void JNICALL JF(mediaPlayerPrepare)(JNIEnv* e, jclass c, jlong handle) {}
JNIEXPORT void JNICALL JF(mediaPlayerStart)(JNIEnv* e, jclass c, jlong handle) {}
JNIEXPORT void JNICALL JF(mediaPlayerPause)(JNIEnv* e, jclass c, jlong handle) {}
JNIEXPORT void JNICALL JF(mediaPlayerStop)(JNIEnv* e, jclass c, jlong handle) {}
JNIEXPORT void JNICALL JF(mediaPlayerRelease)(JNIEnv* e, jclass c, jlong handle) {}
JNIEXPORT void JNICALL JF(mediaPlayerSeekTo)(JNIEnv* e, jclass c, jlong handle, jint msec) {}
JNIEXPORT void JNICALL JF(mediaPlayerReset)(JNIEnv* e, jclass c, jlong handle) {}
JNIEXPORT jint JNICALL JF(mediaPlayerGetDuration)(JNIEnv* e, jclass c, jlong handle) { return 0; }
JNIEXPORT jint JNICALL JF(mediaPlayerGetCurrentPosition)(JNIEnv* e, jclass c, jlong handle) { return 0; }
JNIEXPORT jboolean JNICALL JF(mediaPlayerIsPlaying)(JNIEnv* e, jclass c, jlong handle) { return JNI_FALSE; }
JNIEXPORT void JNICALL JF(mediaPlayerSetVolume)(JNIEnv* e, jclass c, jlong handle, jfloat left, jfloat right) {}
JNIEXPORT void JNICALL JF(mediaPlayerSetLooping)(JNIEnv* e, jclass c, jlong handle, jboolean looping) {}

JNIEXPORT jint JNICALL JF(bitmapWriteToFile)(JNIEnv* e, jclass c, jlong bitmap, jstring path) { return 0; }
JNIEXPORT jint JNICALL JF(bitmapBlitToFb0)(JNIEnv* e, jclass c, jlong bitmap, jint scrollY) { return 0; }
JNIEXPORT jboolean JNICALL JF(vibratorHasVibrator)(JNIEnv* e, jclass c) { return JNI_FALSE; }
JNIEXPORT void JNICALL JF(vibratorVibrate)(JNIEnv* e, jclass c, jlong ms) {}
JNIEXPORT void JNICALL JF(vibratorCancel)(JNIEnv* e, jclass c) {}
JNIEXPORT jint JNICALL JF(checkPermission)(JNIEnv* e, jclass c, jstring permission) { return 0; /* PERMISSION_GRANTED */ }
JNIEXPORT jboolean JNICALL JF(sensorIsAvailable)(JNIEnv* e, jclass c, jint sensorType) { return JNI_FALSE; }
JNIEXPORT jfloatArray JNICALL JF(sensorGetData)(JNIEnv* e, jclass c, jint sensorType) { return ohb_sensor_array(e); }

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
    {"telephonyGetDeviceId","()Ljava/lang/String;",(void*)JF(telephonyGetDeviceId)},
    {"telephonyGetLine1Number","()Ljava/lang/String;",(void*)JF(telephonyGetLine1Number)},
    {"telephonyGetNetworkOperatorName","()Ljava/lang/String;",(void*)JF(telephonyGetNetworkOperatorName)},
    {"telephonyGetSimState","()I",(void*)JF(telephonyGetSimState)},
    {"telephonyGetPhoneType","()I",(void*)JF(telephonyGetPhoneType)},
    {"telephonyGetNetworkType","()I",(void*)JF(telephonyGetNetworkType)},
    {"preferencesOpen","(Ljava/lang/String;)J",(void*)JF(preferencesOpen)},
    {"preferencesGetString","(JLjava/lang/String;Ljava/lang/String;)Ljava/lang/String;",(void*)JF(preferencesGetString)},
    {"preferencesGetInt","(JLjava/lang/String;I)I",(void*)JF(preferencesGetInt)},
    {"preferencesGetLong","(JLjava/lang/String;J)J",(void*)JF(preferencesGetLong)},
    {"preferencesGetFloat","(JLjava/lang/String;F)F",(void*)JF(preferencesGetFloat)},
    {"preferencesGetBoolean","(JLjava/lang/String;Z)Z",(void*)JF(preferencesGetBoolean)},
    {"preferencesPutString","(JLjava/lang/String;Ljava/lang/String;)V",(void*)JF(preferencesPutString)},
    {"preferencesPutInt","(JLjava/lang/String;I)V",(void*)JF(preferencesPutInt)},
    {"preferencesPutLong","(JLjava/lang/String;J)V",(void*)JF(preferencesPutLong)},
    {"preferencesPutFloat","(JLjava/lang/String;F)V",(void*)JF(preferencesPutFloat)},
    {"preferencesPutBoolean","(JLjava/lang/String;Z)V",(void*)JF(preferencesPutBoolean)},
    {"preferencesFlush","(J)V",(void*)JF(preferencesFlush)},
    {"preferencesRemove","(JLjava/lang/String;)V",(void*)JF(preferencesRemove)},
    {"preferencesClear","(J)V",(void*)JF(preferencesClear)},
    {"preferencesClose","(J)V",(void*)JF(preferencesClose)},
    {"rdbStoreOpen","(Ljava/lang/String;I)J",(void*)JF(rdbStoreOpen)},
    {"rdbStoreExecSQL","(JLjava/lang/String;)V",(void*)JF(rdbStoreExecSQL)},
    {"rdbStoreQuery","(JLjava/lang/String;[Ljava/lang/String;)J",(void*)JF(rdbStoreQuery)},
    {"rdbStoreInsert","(JLjava/lang/String;Ljava/lang/String;)J",(void*)JF(rdbStoreInsert)},
    {"rdbStoreUpdate","(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)I",(void*)JF(rdbStoreUpdate)},
    {"rdbStoreDelete","(JLjava/lang/String;Ljava/lang/String;[Ljava/lang/String;)I",(void*)JF(rdbStoreDelete)},
    {"rdbStoreBeginTransaction","(J)V",(void*)JF(rdbStoreBeginTransaction)},
    {"rdbStoreCommit","(J)V",(void*)JF(rdbStoreCommit)},
    {"rdbStoreRollback","(J)V",(void*)JF(rdbStoreRollback)},
    {"rdbStoreClose","(J)V",(void*)JF(rdbStoreClose)},
    {"resultSetGoToFirstRow","(J)Z",(void*)JF(resultSetGoToFirstRow)},
    {"resultSetGoToNextRow","(J)Z",(void*)JF(resultSetGoToNextRow)},
    {"resultSetGetColumnIndex","(JLjava/lang/String;)I",(void*)JF(resultSetGetColumnIndex)},
    {"resultSetGetString","(JI)Ljava/lang/String;",(void*)JF(resultSetGetString)},
    {"resultSetGetInt","(JI)I",(void*)JF(resultSetGetInt)},
    {"resultSetGetLong","(JI)J",(void*)JF(resultSetGetLong)},
    {"resultSetGetFloat","(JI)F",(void*)JF(resultSetGetFloat)},
    {"resultSetGetDouble","(JI)D",(void*)JF(resultSetGetDouble)},
    {"resultSetGetBlob","(JI)[B",(void*)JF(resultSetGetBlob)},
    {"resultSetIsNull","(JI)Z",(void*)JF(resultSetIsNull)},
    {"resultSetGetRowCount","(J)I",(void*)JF(resultSetGetRowCount)},
    {"resultSetGetColumnCount","(J)I",(void*)JF(resultSetGetColumnCount)},
    {"resultSetGetColumnName","(JI)Ljava/lang/String;",(void*)JF(resultSetGetColumnName)},
    {"resultSetClose","(J)V",(void*)JF(resultSetClose)},
    {"notificationPublish","(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V",(void*)JF(notificationPublish)},
    {"notificationCancel","(I)V",(void*)JF(notificationCancel)},
    {"notificationAddSlot","(Ljava/lang/String;Ljava/lang/String;I)V",(void*)JF(notificationAddSlot)},
    {"reminderScheduleTimer","(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",(void*)JF(reminderScheduleTimer)},
    {"reminderCancel","(I)V",(void*)JF(reminderCancel)},
    {"startAbility","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",(void*)JF(startAbility)},
    {"terminateSelf","()V",(void*)JF(terminateSelf)},
    {"logWarn","(Ljava/lang/String;Ljava/lang/String;)V",(void*)JF(logWarn)},
    {"showToast","(Ljava/lang/String;I)V",(void*)JF(showToast)},
    {"httpRequest","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",(void*)JF(httpRequest)},
    {"isNetworkAvailable","()Z",(void*)JF(isNetworkAvailable)},
    {"getNetworkType","()I",(void*)JF(getNetworkType)},
    {"wifiIsEnabled","()Z",(void*)JF(wifiIsEnabled)},
    {"wifiSetEnabled","(Z)Z",(void*)JF(wifiSetEnabled)},
    {"wifiGetState","()I",(void*)JF(wifiGetState)},
    {"wifiGetSSID","()Ljava/lang/String;",(void*)JF(wifiGetSSID)},
    {"wifiGetRssi","()I",(void*)JF(wifiGetRssi)},
    {"wifiGetLinkSpeed","()I",(void*)JF(wifiGetLinkSpeed)},
    {"wifiGetFrequency","()I",(void*)JF(wifiGetFrequency)},
    {"locationGetLast","()[D",(void*)JF(locationGetLast)},
    {"locationIsEnabled","()Z",(void*)JF(locationIsEnabled)},
    {"nodeCreate","(I)J",(void*)JF(nodeCreate)},
    {"nodeDispose","(J)V",(void*)JF(nodeDispose)},
    {"nodeAddChild","(JJ)V",(void*)JF(nodeAddChild)},
    {"nodeRemoveChild","(JJ)V",(void*)JF(nodeRemoveChild)},
    {"nodeInsertChildAt","(JJI)V",(void*)JF(nodeInsertChildAt)},
    {"nodeSetAttrFloat","(JIFFFFI)I",(void*)JF(nodeSetAttrFloat)},
    {"nodeSetAttrColor","(JII)I",(void*)JF(nodeSetAttrColor)},
    {"nodeSetAttrInt","(JII)I",(void*)JF(nodeSetAttrInt)},
    {"nodeSetAttrString","(JILjava/lang/String;)I",(void*)JF(nodeSetAttrString)},
    {"nodeRegisterEvent","(JII)I",(void*)JF(nodeRegisterEvent)},
    {"nodeUnregisterEvent","(JI)V",(void*)JF(nodeUnregisterEvent)},
    {"nodeMarkDirty","(JI)V",(void*)JF(nodeMarkDirty)},
    {"clipboardSet","(Ljava/lang/String;)V",(void*)JF(clipboardSet)},
    {"clipboardGet","()Ljava/lang/String;",(void*)JF(clipboardGet)},
    {"audioGetStreamVolume","(I)I",(void*)JF(audioGetStreamVolume)},
    {"audioGetStreamMaxVolume","(I)I",(void*)JF(audioGetStreamMaxVolume)},
    {"audioSetStreamVolume","(III)V",(void*)JF(audioSetStreamVolume)},
    {"audioGetRingerMode","()I",(void*)JF(audioGetRingerMode)},
    {"audioSetRingerMode","(I)V",(void*)JF(audioSetRingerMode)},
    {"audioIsMusicActive","()Z",(void*)JF(audioIsMusicActive)},
    {"mediaPlayerCreate","()J",(void*)JF(mediaPlayerCreate)},
    {"mediaPlayerSetDataSource","(JLjava/lang/String;)V",(void*)JF(mediaPlayerSetDataSource)},
    {"mediaPlayerPrepare","(J)V",(void*)JF(mediaPlayerPrepare)},
    {"mediaPlayerStart","(J)V",(void*)JF(mediaPlayerStart)},
    {"mediaPlayerPause","(J)V",(void*)JF(mediaPlayerPause)},
    {"mediaPlayerStop","(J)V",(void*)JF(mediaPlayerStop)},
    {"mediaPlayerRelease","(J)V",(void*)JF(mediaPlayerRelease)},
    {"mediaPlayerSeekTo","(JI)V",(void*)JF(mediaPlayerSeekTo)},
    {"mediaPlayerReset","(J)V",(void*)JF(mediaPlayerReset)},
    {"mediaPlayerGetDuration","(J)I",(void*)JF(mediaPlayerGetDuration)},
    {"mediaPlayerGetCurrentPosition","(J)I",(void*)JF(mediaPlayerGetCurrentPosition)},
    {"mediaPlayerIsPlaying","(J)Z",(void*)JF(mediaPlayerIsPlaying)},
    {"mediaPlayerSetVolume","(JFF)V",(void*)JF(mediaPlayerSetVolume)},
    {"mediaPlayerSetLooping","(JZ)V",(void*)JF(mediaPlayerSetLooping)},
    {"bitmapWriteToFile","(JLjava/lang/String;)I",(void*)JF(bitmapWriteToFile)},
    {"bitmapBlitToFb0","(JI)I",(void*)JF(bitmapBlitToFb0)},
    {"vibratorHasVibrator","()Z",(void*)JF(vibratorHasVibrator)},
    {"vibratorVibrate","(J)V",(void*)JF(vibratorVibrate)},
    {"vibratorCancel","()V",(void*)JF(vibratorCancel)},
    {"checkPermission","(Ljava/lang/String;)I",(void*)JF(checkPermission)},
    {"sensorIsAvailable","(I)Z",(void*)JF(sensorIsAvailable)},
    {"sensorGetData","(I)[F",(void*)JF(sensorGetData)},
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
    {"brushDestroy","(J)V",(void*)JF(brushDestroy)},
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

/* PF-arch-013 (2026-05-11): libcore.util.NativeAllocationRegistry.applyFreeFunction.
 * Per Agent C audit: the pc=0x0 SIGBUS signature is this method being called
 * with freeFunction=0. Every JNI handle's GC finalizer goes through this. The
 * default ART implementation does `((void(*)(jlong))freeFunction)(nativePtr)`
 * which faults if freeFunction is 0. We null-guard. */
static void ohb_nar_applyFreeFunction(JNIEnv* e, jclass c, jlong freeFunc, jlong nativePtr) {
    if (freeFunc == 0) return;  /* the whole point of this stub */
    void (*fn)(jlong) = (void (*)(jlong)) (uintptr_t) freeFunc;
    fn(nativePtr);
}

/* PF-arch-013: dalvik.system.VMRuntime — top-priority natives that need stubs
 * before any framework class init triggers GC handle setup. */
static jlong ohb_vmrt_addressOf(JNIEnv* e, jobject t, jobject arr) { return 0; }
static jstring ohb_vmrt_bootClassPath(JNIEnv* e, jobject t) { return (*e)->NewStringUTF(e, ""); }
static void ohb_vmrt_clampGrowthLimit(JNIEnv* e, jobject t) {}
static jstring ohb_vmrt_classPath(JNIEnv* e, jobject t) { return (*e)->NewStringUTF(e, ""); }
static void ohb_vmrt_clearGrowthLimit(JNIEnv* e, jobject t) {}
static jlong ohb_vmrt_getFinalizerTimeoutMs(JNIEnv* e, jobject t) { return 10000; }
static jfloat ohb_vmrt_getTargetHeapUtilization(JNIEnv* e, jobject t) { return 0.75f; }
static jboolean ohb_vmrt_is64Bit(JNIEnv* e, jobject t) { return JNI_TRUE; }
static jboolean ohb_vmrt_isCheckJniEnabled(JNIEnv* e, jobject t) { return JNI_FALSE; }
static jboolean ohb_vmrt_isJavaDebuggable(JNIEnv* e, jobject t) { return JNI_FALSE; }
static jboolean ohb_vmrt_isNativeDebuggable(JNIEnv* e, jobject t) { return JNI_FALSE; }
/* Helper: dispatch on component-type Class to allocate the right primitive
 * array or Object[] array. Real Android's newUnpaddedArray / newNonMovableArray
 * accept primitive component types via Class.isPrimitive(). */
static jobject ohb_vmrt_allocArray(JNIEnv* e, jclass cls, jint len) {
    if (len < 0) len = 0;
    if (cls == NULL) return (*e)->NewObjectArray(e, len, NULL, NULL);
    /* Use getName() to inspect primitive type. Cheaper than reflective
     * Class.isPrimitive() call; getName() returns "int", "long", etc. */
    jclass classCls = (*e)->FindClass(e, "java/lang/Class");
    jmethodID getName = (*e)->GetMethodID(e, classCls, "getName", "()Ljava/lang/String;");
    jstring nameStr = (jstring) (*e)->CallObjectMethod(e, cls, getName);
    if (!nameStr) return (*e)->NewObjectArray(e, len, cls, NULL);
    const char* name = (*e)->GetStringUTFChars(e, nameStr, NULL);
    jobject result = NULL;
    if (name) {
        if (strcmp(name, "int") == 0)       result = (*e)->NewIntArray(e, len);
        else if (strcmp(name, "long") == 0) result = (*e)->NewLongArray(e, len);
        else if (strcmp(name, "byte") == 0) result = (*e)->NewByteArray(e, len);
        else if (strcmp(name, "short") == 0) result = (*e)->NewShortArray(e, len);
        else if (strcmp(name, "char") == 0) result = (*e)->NewCharArray(e, len);
        else if (strcmp(name, "boolean") == 0) result = (*e)->NewBooleanArray(e, len);
        else if (strcmp(name, "float") == 0) result = (*e)->NewFloatArray(e, len);
        else if (strcmp(name, "double") == 0) result = (*e)->NewDoubleArray(e, len);
        else result = (*e)->NewObjectArray(e, len, cls, NULL);
        (*e)->ReleaseStringUTFChars(e, nameStr, name);
    }
    return result;
}
static jobject ohb_vmrt_newNonMovableArray(JNIEnv* e, jobject t, jclass cls, jint len) {
    return ohb_vmrt_allocArray(e, cls, len);
}
static jobject ohb_vmrt_newUnpaddedArray(JNIEnv* e, jobject t, jclass cls, jint len) {
    return ohb_vmrt_allocArray(e, cls, len);
}
static void ohb_vmrt_bootCompleted(JNIEnv* e, jclass c) {}
static jstring ohb_vmrt_getCurrentInstructionSet(JNIEnv* e, jclass c) { return (*e)->NewStringUTF(e, "arm64"); }
static jint ohb_vmrt_getNotifyNativeInterval(JNIEnv* e, jclass c) { return 0; }
static jint ohb_vmrt_getSdkVersionNative(JNIEnv* e, jclass c, jint def) { return 35; }
static jboolean ohb_vmrt_isBootClassPathOnDisk(JNIEnv* e, jclass c, jstring s) { return JNI_FALSE; }
static jboolean ohb_vmrt_isValidClassLoaderContext(JNIEnv* e, jclass c, jstring s) { return JNI_TRUE; }
static void ohb_vmrt_nativeSetTargetHeapUtilization(JNIEnv* e, jclass c, jfloat f) {}
static void ohb_vmrt_registerAppInfo(JNIEnv* e, jclass c, jstring a, jstring b, jstring d, jobjectArray e2, jint i) {}
static void ohb_vmrt_registerSensitiveThread(JNIEnv* e, jclass c) {}
static void ohb_vmrt_resetJitCounters(JNIEnv* e, jclass c) {}
static void ohb_vmrt_setDedupeHiddenApiWarnings(JNIEnv* e, jclass c, jboolean b) {}
static void ohb_vmrt_setDisabledCompatChangesNative(JNIEnv* e, jclass c, jlongArray a) {}
static void ohb_vmrt_setProcessDataDirectory(JNIEnv* e, jclass c, jstring s) {}
static void ohb_vmrt_setProcessPackageName(JNIEnv* e, jclass c, jstring s) {}
static void ohb_vmrt_setSystemDaemonThreadPriority(JNIEnv* e, jclass c) {}
static void ohb_vmrt_setTargetSdkVersionNative(JNIEnv* e, jclass c, jint i) {}

/* PF-arch-009 (2026-05-11): dalvik.system.VMStack native stubs.
 * Used internally by Thread.getStackTrace() and by ClassLoader error-message
 * construction. Unregistered → fault_addr=0x0 SIGBUS during ClassNotFoundException
 * message build. Stubs return null/empty/zero which is fine for diagnostic paths. */
static jobjectArray ohb_vmstack_getThreadStackTrace(JNIEnv* e, jclass c, jobject thread) {
    /* Return empty StackTraceElement[] — error messages get [] instead of real trace. */
    jclass steCls = (*e)->FindClass(e, "java/lang/StackTraceElement");
    if (!steCls) { (*e)->ExceptionClear(e); return NULL; }
    return (*e)->NewObjectArray(e, 0, steCls, NULL);
}
static jint ohb_vmstack_fillStackTraceElements(JNIEnv* e, jclass c, jobject thread, jobjectArray arr) {
    return 0;  /* nothing filled */
}
static jobjectArray ohb_vmstack_getAnnotatedThreadStackTrace(JNIEnv* e, jclass c, jobject thread) {
    return NULL;
}
static jobject ohb_vmstack_getCallingClassLoader(JNIEnv* e, jclass c) { return NULL; }
static jobject ohb_vmstack_getClosestUserClassLoader(JNIEnv* e, jclass c) { return NULL; }
static jclass  ohb_vmstack_getStackClass2(JNIEnv* e, jclass c) { return NULL; }
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
    fprintf(stderr, "[PF202N] OHBridge_JNI_OnLoad_Impl entry vm=%p reserved=%p\n", vm, reserved);
    fflush(stderr);
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
    fprintf(stderr, "[PF202N] OHBridge_JNI_OnLoad_Impl OHBridge register ok=%d count=%d pipe_fd=%d\n", ok, count, pipe_fd);
    fflush(stderr);

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

        /* PF-arch-013: libcore.util.NativeAllocationRegistry — null-guard the
         * free-function trampoline that's at the heart of every JNI handle's
         * GC finalizer. Per Agent C audit, the pc=0 SIGBUS in startActivity
         * is this native being called with freeFunc=0. */
        jclass narCls = (*env)->FindClass(env, "libcore/util/NativeAllocationRegistry");
        if (narCls) {
            JNINativeMethod nar[] = {
                {"applyFreeFunction", "(JJ)V", (void*)ohb_nar_applyFreeFunction},
            };
            int rc = (*env)->RegisterNatives(env, narCls, &nar[0], 1);
            (*env)->ExceptionClear(env);
            fprintf(stderr, "[OHBridge] NativeAllocationRegistry stub: %s\n", rc == 0 ? "OK" : "FAIL");
        } else { (*env)->ExceptionClear(env); }

        /* PF-arch-013: dalvik.system.VMRuntime — heap/sdk/process settings */
        jclass vmrtCls = (*env)->FindClass(env, "dalvik/system/VMRuntime");
        if (vmrtCls) {
            JNINativeMethod vmrt[] = {
                {"addressOf", "(Ljava/lang/Object;)J", (void*)ohb_vmrt_addressOf},
                {"bootClassPath", "()Ljava/lang/String;", (void*)ohb_vmrt_bootClassPath},
                {"clampGrowthLimit", "()V", (void*)ohb_vmrt_clampGrowthLimit},
                {"classPath", "()Ljava/lang/String;", (void*)ohb_vmrt_classPath},
                {"clearGrowthLimit", "()V", (void*)ohb_vmrt_clearGrowthLimit},
                {"getFinalizerTimeoutMs", "()J", (void*)ohb_vmrt_getFinalizerTimeoutMs},
                {"getTargetHeapUtilization", "()F", (void*)ohb_vmrt_getTargetHeapUtilization},
                {"is64Bit", "()Z", (void*)ohb_vmrt_is64Bit},
                {"isCheckJniEnabled", "()Z", (void*)ohb_vmrt_isCheckJniEnabled},
                {"isJavaDebuggable", "()Z", (void*)ohb_vmrt_isJavaDebuggable},
                {"isNativeDebuggable", "()Z", (void*)ohb_vmrt_isNativeDebuggable},
                {"newNonMovableArray", "(Ljava/lang/Class;I)Ljava/lang/Object;", (void*)ohb_vmrt_newNonMovableArray},
                {"newUnpaddedArray", "(Ljava/lang/Class;I)Ljava/lang/Object;", (void*)ohb_vmrt_newUnpaddedArray},
                {"bootCompleted", "()V", (void*)ohb_vmrt_bootCompleted},
                {"getCurrentInstructionSet", "()Ljava/lang/String;", (void*)ohb_vmrt_getCurrentInstructionSet},
                {"getNotifyNativeInterval", "()I", (void*)ohb_vmrt_getNotifyNativeInterval},
                {"getSdkVersionNative", "(I)I", (void*)ohb_vmrt_getSdkVersionNative},
                {"isBootClassPathOnDisk", "(Ljava/lang/String;)Z", (void*)ohb_vmrt_isBootClassPathOnDisk},
                {"isValidClassLoaderContext", "(Ljava/lang/String;)Z", (void*)ohb_vmrt_isValidClassLoaderContext},
                {"nativeSetTargetHeapUtilization", "(F)V", (void*)ohb_vmrt_nativeSetTargetHeapUtilization},
                {"registerAppInfo", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;I)V", (void*)ohb_vmrt_registerAppInfo},
                {"registerSensitiveThread", "()V", (void*)ohb_vmrt_registerSensitiveThread},
                {"resetJitCounters", "()V", (void*)ohb_vmrt_resetJitCounters},
                {"setDedupeHiddenApiWarnings", "(Z)V", (void*)ohb_vmrt_setDedupeHiddenApiWarnings},
                {"setDisabledCompatChangesNative", "([J)V", (void*)ohb_vmrt_setDisabledCompatChangesNative},
                {"setProcessDataDirectory", "(Ljava/lang/String;)V", (void*)ohb_vmrt_setProcessDataDirectory},
                {"setProcessPackageName", "(Ljava/lang/String;)V", (void*)ohb_vmrt_setProcessPackageName},
                {"setSystemDaemonThreadPriority", "()V", (void*)ohb_vmrt_setSystemDaemonThreadPriority},
                {"setTargetSdkVersionNative", "(I)V", (void*)ohb_vmrt_setTargetSdkVersionNative},
            };
            int n = sizeof(vmrt)/sizeof(vmrt[0]);
            int i, ok = 0;
            for (i = 0; i < n; i++) {
                if ((*env)->RegisterNatives(env, vmrtCls, &vmrt[i], 1) == 0) ok++;
                else (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] VMRuntime stubs: %d/%d\n", ok, n);
        } else { (*env)->ExceptionClear(env); }

        /* PF-arch-009: dalvik.system.VMStack — used by Thread.getStackTrace() */
        jclass vmStackCls = (*env)->FindClass(env, "dalvik/system/VMStack");
        if (vmStackCls) {
            JNINativeMethod vm[] = {
                {"getThreadStackTrace", "(Ljava/lang/Thread;)[Ljava/lang/StackTraceElement;", (void*)ohb_vmstack_getThreadStackTrace},
                {"fillStackTraceElements", "(Ljava/lang/Thread;[Ljava/lang/StackTraceElement;)I", (void*)ohb_vmstack_fillStackTraceElements},
                {"getAnnotatedThreadStackTrace", "(Ljava/lang/Thread;)[Ldalvik/system/AnnotatedStackTraceElement;", (void*)ohb_vmstack_getAnnotatedThreadStackTrace},
                {"getCallingClassLoader", "()Ljava/lang/ClassLoader;", (void*)ohb_vmstack_getCallingClassLoader},
                {"getClosestUserClassLoader", "()Ljava/lang/ClassLoader;", (void*)ohb_vmstack_getClosestUserClassLoader},
                {"getStackClass2", "()Ljava/lang/Class;", (void*)ohb_vmstack_getStackClass2},
            };
            int i, vm_ok = 0;
            for (i = 0; i < 6; i++) {
                if ((*env)->RegisterNatives(env, vmStackCls, &vm[i], 1) == 0) vm_ok++;
                else (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] VMStack stubs: %d/6\n", vm_ok);
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

        /* SystemClock — register all 6 entries with per-method status. */
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
            int sc_ok = 0;
            int n_sc = (int)(sizeof(scM)/sizeof(scM[0]));
            int i; for (i = 0; i < n_sc; i++) {
                jint r = (*env)->RegisterNatives(env, scCls, &scM[i], 1);
                if (r == 0) sc_ok++;
                else {
                    fprintf(stderr, "[OHBridge] SystemClock register FAIL: %s%s (r=%d)\n",
                            scM[i].name, scM[i].signature, (int)r);
                }
                (*env)->ExceptionClear(env);
            }
            fprintf(stderr, "[OHBridge] SystemClock stubs: %d/%d\n", sc_ok, n_sc);
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
            int n_am = (int)(sizeof(amM)/sizeof(amM[0]));
            for (i = 0; i < n_am; i++) {
                if ((*env)->RegisterNatives(env, amCls, &amM[i], 1) == 0) {
                    am_ok++;
                } else {
                    fprintf(stderr, "[OHBridge] AssetManager FAIL: %s%s\n", amM[i].name, amM[i].signature);
                    (*env)->ExceptionClear(env);
                }
            }
            fprintf(stderr, "[OHBridge] AssetManager stubs: %d/%d\n", am_ok, n_am);
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
