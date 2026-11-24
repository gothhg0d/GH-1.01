/* Hardware (sceGu) 2D renderer: everything is drawn by the PSP GPU. */
#pragma once
#include <psptypes.h>

/* font ids - same order as FONTS in tools/make_font.py (font_data.h is only
   included by gfx.c, so the atlas exists once) */
#ifndef F_SMALL
#define F_SMALL   0  /* Regular 10   */
#define F_BODY    1  /* Regular 12   */
#define F_LABEL   2  /* Semibold 13  */
#define F_TITLE   3  /* Semibold 20  */
#define F_DISPLAY 4  /* Bold 30      */
#define F_LOGO    5  /* Heavy 52, only " PSL0-9" */
#endif

#define SCR_W 480
#define SCR_H 272

typedef u32 Color; /* 0xAABBGGRR, as the PSP GPU wants it */

/* constant expressions, usable in static initializers */
#define RGBA(r, g, b, a) ((Color)(((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r)))
#define RGB(r, g, b) RGBA(r, g, b, 255)
static inline int colA(Color c) { return c >> 24; }

Color colFade(Color c, float a);           /* multiply alpha by a (0..1) */
Color colLerp(Color a, Color b, float t);  /* per channel, including alpha */
Color colWithA(Color c, int a);

/* PlayStation button glyphs (UTF-8 for U+E000..U+E003) */
#define G_CROSS    "\xEE\x80\x80"
#define G_CIRCLE   "\xEE\x80\x81"
#define G_TRIANGLE "\xEE\x80\x82"
#define G_SQUARE   "\xEE\x80\x83"

typedef struct {
    void *data;       /* 16-byte aligned, in RAM (written back) or VRAM */
    int w, h;         /* used size */
    int tw, th;       /* power-of-two size */
    int stride;       /* buffer width in pixels */
    int psm;          /* GU_PSM_8888 / GU_PSM_5650 */
} Texture;

void gfxInit(void);
void gfxBeginFrame(Color clear);
void gfxEndFrame(void);
void gfxFinish(void);   /* end drawing without swap (frame timing) */
void gfxSwap(void);

void *gfxVramUncached(u32 offset);       /* CPU pointer into VRAM */
void gfxArtSlot(int slot, Texture *t);   /* 480x272 5650 art slots in VRAM */
u16  *gfxArtSlotPixels(int slot);

void gfxClip(int x, int y, int w, int h);
void gfxNoClip(void);

void gfxRect(float x, float y, float w, float h, Color c);
void gfxGradV(float x, float y, float w, float h, Color top, Color bot);
void gfxGradH(float x, float y, float w, float h, Color left, Color right);
void gfxRound(float x, float y, float w, float h, float r, Color c);
void gfxRoundGradV(float x, float y, float w, float h, float r, Color top, Color bot);
void gfxRing(float x, float y, float w, float h, float r, Color c); /* outline ~r*0.19 px thick */
void gfxShadow(float x, float y, float w, float h, float blur, Color c);
void gfxCircle(float cx, float cy, float r, Color c);
void gfxGlow(float cx, float cy, float r, Color c);
void gfxCircles(const float *x, const float *y, const float *r, const Color *c, int n);

void gfxImage(const Texture *t, float x, float y, float w, float h, Color tint);
void gfxCornerMask(float x, float y, float w, float h, float r, Color c);

/* text: y is the top of the line box */
float gfxTextW(int font, const char *s);
int   gfxLineH(int font);
void  gfxText(int font, float x, float y, Color c, const char *s);
void  gfxTextC(int font, float cx, float y, Color c, const char *s);
void  gfxTextR(int font, float rx, float y, Color c, const char *s);
void  gfxTextScaled(int font, float cx, float cy, float scale, Color c, const char *s); /* centered */
void  gfxTextFit(int font, float x, float y, float maxW, Color c, const char *s);    /* adds ... */
