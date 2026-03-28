/*
 * OHBridge JNI stub — subprocess display list mode.
 * Writes Canvas ops to mmap'd shared memory (version=1 pixel buffer).
 * Entry point: JNI_OnLoad (renamed to JNI_OnLoad_ohbridge via -D)
 */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SHM_HDR 128
#define DLIST_MAX (512*1024)
#define SHM_TOTAL (SHM_HDR + DLIST_MAX + 64)

static unsigned char* shm = NULL;
static int shm_pos = 0, shm_seq = 0;
static JavaVM* g_vm = NULL;

static void emit1(unsigned char v) { if(shm && shm_pos<DLIST_MAX-64) shm[SHM_HDR+shm_pos++]=v; }
static void emit4(const void* v) { if(shm && shm_pos+4<=DLIST_MAX-64){memcpy(shm+SHM_HDR+shm_pos,v,4);shm_pos+=4;} }
static void emitf(float v) { emit4(&v); }
static void emiti(int v) { emit4(&v); }
static void emit2(short v) { if(shm && shm_pos+2<=DLIST_MAX-64){memcpy(shm+SHM_HDR+shm_pos,&v,2);shm_pos+=2;} }

enum { OP_COLOR=1,OP_RECT=2,OP_TEXT=3,OP_LINE=4,OP_SAVE=5,OP_RESTORE=6,OP_TRANSLATE=7,OP_CLIP=8,OP_RRECT=9,OP_CIRCLE=10 };

#define MAX_H 256
static int h_colors[MAX_H];
static float h_fontsz[MAX_H];
static int h_next = 1;
static int idx(long h) { return (int)(h & 0xFF); }

static void shm_init() {
    const char* p = getenv("WESTLAKE_SHM");
    if(!p||!p[0]) p="/data/local/tmp/westlake/westlake_shm";
    int fd = open(p, O_RDWR);
    if(fd<0){printf("[OHBridge] shm open FAIL\n");return;}
    if(lseek(fd,0,SEEK_END)<SHM_TOTAL) ftruncate(fd,SHM_TOTAL);
    shm = mmap(NULL,SHM_TOTAL,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(shm==MAP_FAILED){shm=NULL;return;}
    *(int*)(shm+0)=0x574C4B46; *(int*)(shm+4)=2;
    *(int*)(shm+8)=480; *(int*)(shm+12)=800;
    msync(shm,SHM_HDR,MS_SYNC);
    printf("[OHBridge] shm_init OK (mmap display list)\n"); fflush(stdout);
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_vm = vm;
    printf("[OHBridge] JNI_OnLoad (display list stub)\n"); fflush(stdout);
    return JNI_VERSION_1_6;
}

/* === JNI exports (Java_com_ohos_shim_bridge_OHBridge_*) === */
#define JF(name) Java_com_ohos_shim_bridge_OHBridge_##name

JNIEXPORT jint JNICALL JF(arkuiInit)(JNIEnv* e, jclass c) { shm_init(); return 0; }
JNIEXPORT jlong JNICALL JF(surfaceCreate)(JNIEnv* e, jclass c, jlong u, jint w, jint h) { return 1; }
JNIEXPORT jlong JNICALL JF(surfaceGetCanvas)(JNIEnv* e, jclass c, jlong s) { shm_pos=0; return 1; }
JNIEXPORT jint JNICALL JF(surfaceFlush)(JNIEnv* e, jclass c, jlong s) {
    if(!shm) return -1;
    shm_seq++;
    *(int*)(shm+20)=shm_pos;
    __sync_synchronize();
    *(int*)(shm+16)=shm_seq;
    msync(shm,SHM_HDR+shm_pos,MS_SYNC);
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
    if(len>0 && shm_pos+19+len<DLIST_MAX-64) {
        emit1(OP_TEXT); emitf(x); emitf(y); emitf(h_fontsz[idx(font)]);
        emiti(h_colors[idx(pen>0?pen:brush)]); emit2((short)len);
        memcpy(shm+SHM_HDR+shm_pos,u,len); shm_pos+=len;
    }
    if(u) (*e)->ReleaseStringUTFChars(e,text,u);
}
JNIEXPORT void JNICALL JF(canvasSave)(JNIEnv* e, jclass c, jlong cn) { emit1(OP_SAVE); }
JNIEXPORT void JNICALL JF(canvasRestore)(JNIEnv* e, jclass c, jlong cn) { emit1(OP_RESTORE); }
JNIEXPORT void JNICALL JF(canvasTranslate)(JNIEnv* e, jclass c, jlong cn, jfloat dx, jfloat dy) { emit1(OP_TRANSLATE); emitf(dx); emitf(dy); }
JNIEXPORT void JNICALL JF(canvasScale)(JNIEnv* e, jclass c, jlong cn, jfloat sx, jfloat sy) {}
JNIEXPORT void JNICALL JF(canvasClipRect)(JNIEnv* e, jclass c, jlong cn, jfloat l, jfloat t, jfloat r, jfloat b2) { emit1(OP_CLIP); emitf(l); emitf(t); emitf(r); emitf(b2); }
JNIEXPORT void JNICALL JF(canvasDrawPath)(JNIEnv* e, jclass c, jlong cn, jlong path, jlong pen, jlong brush) {}
JNIEXPORT void JNICALL JF(canvasDrawBitmap)(JNIEnv* e, jclass c, jlong cn, jlong bmp, jfloat x, jfloat y) {}
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

JNIEXPORT jlong JNICALL JF(bitmapCreate)(JNIEnv* e, jclass c, jint w, jint h, jint fmt) { return 1; }
JNIEXPORT void JNICALL JF(bitmapDestroy)(JNIEnv* e, jclass c, jlong b) {}
JNIEXPORT jint JNICALL JF(bitmapGetWidth)(JNIEnv* e, jclass c, jlong b) { return 480; }
JNIEXPORT jint JNICALL JF(bitmapGetHeight)(JNIEnv* e, jclass c, jlong b) { return 800; }
JNIEXPORT void JNICALL JF(bitmapSetPixel)(JNIEnv* e, jclass c, jlong b, jint x, jint y, jint col) {}
JNIEXPORT jint JNICALL JF(bitmapGetPixel)(JNIEnv* e, jclass c, jlong b, jint x, jint y) { return 0; }

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
