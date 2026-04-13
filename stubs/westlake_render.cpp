// Westlake direct screen renderer — minimal, no AOSP header dependencies
// Uses the phone's libgui.so + libui.so via a thin wrapper
// Approach: compile a helper .so that wraps SurfaceComposerClient in C API,
// then call it from our renderer.
//
// ALTERNATIVE: use /dev/graphics/fb0 (old devices) or hwcomposer HAL
//
// SIMPLEST: use `screencap` to read current framebuffer, modify it, and
// write back via service call. But that's not real-time.
//
// ACTUAL APPROACH: compile this ON THE PHONE using the phone's own compiler
// and link against the phone's libgui.so directly. This avoids header issues.

// For now: render to raw RGBA file, then convert to PNG and push to a
// simple viewer. This proves the rendering pipeline works.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RGBA(r,g,b) ((uint32_t)0xFF000000|((uint32_t)(b)<<16)|((uint32_t)(g)<<8)|(uint32_t)(r))
#define MCD_RED   RGBA(0xDA,0x29,0x1C)
#define MCD_GOLD  RGBA(0xFF,0xC7,0x2C)
#define MCD_BROWN RGBA(0x27,0x25,0x1F)
#define MCD_WHITE RGBA(0xFF,0xFF,0xFF)

static void fill(uint32_t*b,int s,int x,int y,int w,int h,int W,int H,uint32_t c){
    for(int r=y;r<y+h&&r<H;r++)for(int co=x;co<x+w&&co<W;co++)if(r>=0&&co>=0)b[r*s+co]=c;}
static void thickLine(uint32_t*b,int s,int x0,int y0,int x1,int y1,int W,int H,int t,uint32_t c){
    int dx=abs(x1-x0),dy=abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,e=dx-dy;
    while(1){fill(b,s,x0-t/2,y0-t/2,t,t,W,H,c);if(x0==x1&&y0==y1)break;
    int e2=2*e;if(e2>-dy){e-=dy;x0+=sx;}if(e2<dx){e+=dx;y0+=sy;}}}

int main() {
    int W = 1080, H = 2280;
    uint32_t* px = (uint32_t*)calloc(W*H, 4);
    if (!px) return 1;

    // Draw MCD splash
    for(int i=0;i<W*H;i++) px[i]=MCD_BROWN;
    int cx=W/2,cy=H/3,ah=H/5,aw=W/4,th=W/25;
    thickLine(px,W,cx-aw,cy+ah/2,cx-aw/2,cy-ah/2,W,H,th,MCD_GOLD);
    thickLine(px,W,cx-aw/2,cy-ah/2,cx-aw/6,cy+ah/3,W,H,th,MCD_GOLD);
    thickLine(px,W,cx+aw/6,cy+ah/3,cx+aw/2,cy-ah/2,W,H,th,MCD_GOLD);
    thickLine(px,W,cx+aw/2,cy-ah/2,cx+aw,cy+ah/2,W,H,th,MCD_GOLD);
    fill(px,W,0,H*60/100,W,H/10,W,H,MCD_RED);
    fill(px,W,W/5,H*73/100,W*3/5,H/12,W,H,MCD_WHITE);

    // Write raw RGBA
    FILE* f = fopen("/data/local/tmp/westlake/mcd_splash.rgba","wb");
    if(f){fwrite(px,4,W*H,f);fclose(f);}

    // Convert to PNG using screencap trick:
    // 1. Read current framebuffer via screencap
    // 2. Replace its pixels with ours
    // 3. ???
    // Actually, just write the raw pixels to stdout in screencap format
    // Header: width(4) height(4) format(4)
    uint32_t header[3] = {(uint32_t)W, (uint32_t)H, 1/*RGBA_8888*/};
    write(1, header, 12);
    write(1, px, W*H*4);

    free(px);
    return 0;
}
