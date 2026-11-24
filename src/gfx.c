#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <string.h>
#include <math.h>
#include "font_data.h"
#include "gfx.h"

/* VRAM layout (2 MB):
 *   0x000000 front/back framebuffers 512x272x4 each
 *   0x110000 font+shape atlas 512x512 T8 (256 KB)
 *   0x150000 two 480x272 RGB565 art slots for PIC1 backgrounds (255 KB each) */
#define BUF_W     512
#define FB_SIZE   (BUF_W * SCR_H * 4)
#define ATLAS_OFS (FB_SIZE * 2)
#define ART_OFS   (ATLAS_OFS + ATLAS_W * ATLAS_H)
#define ART_SIZE  (480 * 272 * 2)

static unsigned int __attribute__((aligned(64))) dlist[262144];
static u32 __attribute__((aligned(16))) clut[256];
static int texIsAtlas;
static int curFilter;

typedef struct { float u, v; u32 c; float x, y, z; } V;
#define VFMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

/* ---------------------------------------------------------------- colors */
Color colFade(Color c, float a) {
    if (a >= 1.0f) return c;
    if (a <= 0.0f) return c & 0x00FFFFFF;
    return (c & 0x00FFFFFF) | ((u32)(colA(c) * a) << 24);
}
Color colWithA(Color c, int a) {
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (c & 0x00FFFFFF) | ((u32)a << 24);
}
Color colLerp(Color a, Color b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    Color o = 0;
    for (int s = 0; s < 32; s += 8) {
        int ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        o |= (u32)(ca + (int)((cb - ca) * t)) << s;
    }
    return o;
}

/* ---------------------------------------------------------------- setup */
void *gfxVramUncached(u32 ofs) { return (void *)(0x44000000 + ofs); }
static void *vramGe(u32 ofs) { return (void *)(0x04000000 + ofs); }

void gfxArtSlot(int slot, Texture *t) {
    t->data = vramGe(ART_OFS + slot * ART_SIZE);
    t->w = 480; t->h = 272; t->tw = 512; t->th = 512; t->stride = 480;
    t->psm = GU_PSM_5650;
}
u16 *gfxArtSlotPixels(int slot) { return (u16 *)gfxVramUncached(ART_OFS + slot * ART_SIZE); }

static void bindAtlas(void) {
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, vramGe(ATLAS_OFS));
    sceGuTexFlush();
    texIsAtlas = 1;
}
static inline void useAtlas(void) { if (!texIsAtlas) bindAtlas(); }
static inline void filter(int linear) {
    if (curFilter != linear) {
        sceGuTexFilter(linear ? GU_LINEAR : GU_NEAREST, linear ? GU_LINEAR : GU_NEAREST);
        curFilter = linear;
    }
}

void gfxInit(void) {
    memcpy(gfxVramUncached(ATLAS_OFS), atlas_data, sizeof(atlas_data));
    memset(gfxVramUncached(ART_OFS), 0, ART_SIZE * 2);
    for (int i = 0; i < 256; i++) clut[i] = ((u32)i << 24) | 0x00FFFFFF;
    sceKernelDcacheWritebackAll();

    sceGuInit();
    sceGuStart(GU_DIRECT, dlist);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)FB_SIZE, BUF_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, clut);
    bindAtlas();
    curFilter = -1;
    filter(1);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/* full render state, re-applied every frame (system dialogs like the OSK
   change GPU state behind our back) */
static void applyState(void) {
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexOffset(0, 0);
    sceGuTexScale(1, 1);
}

/* for system dialogs: finish our drawing without swapping */
void gfxFinish(void) {
    sceGuFinish();
    sceGuSync(0, 0);
}
void gfxSwap(void) {
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

void gfxBeginFrame(Color clear) {
    sceGuStart(GU_DIRECT, dlist);
    applyState();
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, clut);
    bindAtlas();
    curFilter = -1;
    filter(1);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuClearColor(clear);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

void gfxEndFrame(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

void gfxClip(int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    if (x1 <= x || y1 <= y) { x = y = 0; x1 = y1 = 1; }
    sceGuScissor(x, y, x1, y1); /* pspsdk takes right/bottom here */
}
void gfxNoClip(void) { sceGuScissor(0, 0, SCR_W, SCR_H); }

/* ---------------------------------------------------------------- shapes */
static inline V *verts(int n) { return (V *)sceGuGetMemory(n * sizeof(V)); }
static inline void sv(V *v, float u, float t, u32 c, float x, float y) {
    v->u = u; v->v = t; v->c = c; v->x = x; v->y = y; v->z = 0;
}

/* opaque texel in the middle of the 64px disk */
#define SOLID_U (SHAPE_DISK64_X + 32.0f)
#define SOLID_V (SHAPE_DISK64_Y + 32.0f)

void gfxRect(float x, float y, float w, float h, Color c) {
    if (w <= 0 || h <= 0 || !colA(c)) return;
    useAtlas(); filter(1);
    V *v = verts(2);
    sv(&v[0], SOLID_U, SOLID_V, c, x, y);
    sv(&v[1], SOLID_U, SOLID_V, c, x + w, y + h);
    sceGuDrawArray(GU_SPRITES, VFMT, 2, 0, v);
}

void gfxGradV(float x, float y, float w, float h, Color top, Color bot) {
    if (w <= 0 || h <= 0) return;
    useAtlas(); filter(1);
    V *v = verts(4);
    sv(&v[0], SOLID_U, SOLID_V, top, x, y);
    sv(&v[1], SOLID_U, SOLID_V, top, x + w, y);
    sv(&v[2], SOLID_U, SOLID_V, bot, x, y + h);
    sv(&v[3], SOLID_U, SOLID_V, bot, x + w, y + h);
    sceGuDrawArray(GU_TRIANGLE_STRIP, VFMT, 4, 0, v);
}

void gfxGradH(float x, float y, float w, float h, Color left, Color right) {
    if (w <= 0 || h <= 0) return;
    useAtlas(); filter(1);
    V *v = verts(4);
    sv(&v[0], SOLID_U, SOLID_V, left, x, y);
    sv(&v[1], SOLID_U, SOLID_V, right, x + w, y);
    sv(&v[2], SOLID_U, SOLID_V, left, x, y + h);
    sv(&v[3], SOLID_U, SOLID_V, right, x + w, y + h);
    sceGuDrawArray(GU_TRIANGLE_STRIP, VFMT, 4, 0, v);
}

/* 9-slice: corners come from a quarter of a round texture of size ts at (tx,ty) */
static void nine(float x, float y, float w, float h, float r, float tx, float ty, float ts,
                 Color top, Color bot) {
    if (w <= 0 || h <= 0) return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    float xs[4] = {x, x + r, x + w - r, x + w};
    float ys[4] = {y, y + r, y + h - r, y + h};
    float hs = ts * 0.5f;
    float us[4] = {tx, tx + hs, tx + hs, tx + ts};
    float vs[4] = {ty, ty + hs, ty + hs, ty + ts};
    Color cs[4];
    for (int j = 0; j < 4; j++) cs[j] = (top == bot) ? top : colLerp(top, bot, (ys[j] - y) / h);
    useAtlas(); filter(1);
    V *v = verts(54);
    int k = 0;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            V a, b, c, d;
            sv(&a, us[i], vs[j], cs[j], xs[i], ys[j]);
            sv(&b, us[i + 1], vs[j], cs[j], xs[i + 1], ys[j]);
            sv(&c, us[i], vs[j + 1], cs[j + 1], xs[i], ys[j + 1]);
            sv(&d, us[i + 1], vs[j + 1], cs[j + 1], xs[i + 1], ys[j + 1]);
            v[k++] = a; v[k++] = b; v[k++] = c;
            v[k++] = b; v[k++] = d; v[k++] = c;
        }
    sceGuDrawArray(GU_TRIANGLES, VFMT, 54, 0, v);
}

void gfxRoundGradV(float x, float y, float w, float h, float r, Color top, Color bot) {
    if (!colA(top) && !colA(bot)) return;
    if (r < 0.5f) { gfxGradV(x, y, w, h, top, bot); return; }
    if (r <= 6.0f) nine(x, y, w, h, r, SHAPE_DISK16_X, SHAPE_DISK16_Y, 16, top, bot);
    else nine(x, y, w, h, r, SHAPE_DISK64_X, SHAPE_DISK64_Y, 64, top, bot);
}
void gfxRound(float x, float y, float w, float h, float r, Color c) { gfxRoundGradV(x, y, w, h, r, c, c); }

void gfxRing(float x, float y, float w, float h, float r, Color c) {
    if (!colA(c)) return;
    nine(x, y, w, h, r, SHAPE_RING64_X, SHAPE_RING64_Y, 64, c, c);
}

void gfxShadow(float x, float y, float w, float h, float blur, Color c) {
    if (!colA(c)) return;
    nine(x - blur, y - blur, w + blur * 2, h + blur * 2, blur * 2, SHAPE_BLOB64_X, SHAPE_BLOB64_Y, 64, c, c);
}

static void shapeSprite(float tx, float ty, float ts, float cx, float cy, float r, Color c) {
    if (!colA(c) || r <= 0) return;
    useAtlas(); filter(1);
    V *v = verts(2);
    sv(&v[0], tx, ty, c, cx - r, cy - r);
    sv(&v[1], tx + ts, ty + ts, c, cx + r, cy + r);
    sceGuDrawArray(GU_SPRITES, VFMT, 2, 0, v);
}
void gfxCircle(float cx, float cy, float r, Color c) {
    if (r <= 6) shapeSprite(SHAPE_DISK16_X, SHAPE_DISK16_Y, 16, cx, cy, r, c);
    else shapeSprite(SHAPE_DISK64_X, SHAPE_DISK64_Y, 64, cx, cy, r, c);
}
void gfxGlow(float cx, float cy, float r, Color c) { shapeSprite(SHAPE_BLOB64_X, SHAPE_BLOB64_Y, 64, cx, cy, r, c); }

/* many small circles in one draw call (particles) */
void gfxCircles(const float *x, const float *y, const float *r, const Color *c, int n) {
    if (n <= 0) return;
    useAtlas(); filter(1);
    V *v = verts(n * 2);
    for (int i = 0; i < n; i++) {
        float tx = SHAPE_DISK16_X, ty = SHAPE_DISK16_Y;
        sv(&v[i * 2], tx, ty, c[i], x[i] - r[i], y[i] - r[i]);
        sv(&v[i * 2 + 1], tx + 16, ty + 16, c[i], x[i] + r[i], y[i] + r[i]);
    }
    sceGuDrawArray(GU_SPRITES, VFMT, n * 2, 0, v);
}

/* paints the 4 corners outside a rounded rect with c: rounds images drawn on
   an opaque panel of color c */
void gfxCornerMask(float x, float y, float w, float h, float r, Color c) {
    if (r < 0.5f || !colA(c)) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    const float u0 = SHAPE_CORNER32_X, v0 = SHAPE_CORNER32_Y, u1 = u0 + 32, v1 = v0 + 32;
    useAtlas(); filter(1);
    V *v = verts(8);
    sv(&v[0], u0, v0, c, x, y);                 sv(&v[1], u1, v1, c, x + r, y + r);         /* TL */
    sv(&v[2], u1, v0, c, x + w - r, y);         sv(&v[3], u0, v1, c, x + w, y + r);         /* TR */
    sv(&v[4], u0, v1, c, x, y + h - r);         sv(&v[5], u1, v0, c, x + r, y + h);         /* BL */
    sv(&v[6], u1, v1, c, x + w - r, y + h - r); sv(&v[7], u0, v0, c, x + w, y + h);         /* BR */
    sceGuDrawArray(GU_SPRITES, VFMT, 8, 0, v);
}

/* ---------------------------------------------------------------- images */
void gfxImage(const Texture *t, float x, float y, float w, float h, Color tint) {
    if (!t || !t->data || w <= 0 || h <= 0 || !colA(tint)) return;
    sceGuTexMode(t->psm, 0, 0, 0);
    sceGuTexImage(0, t->tw, t->th, t->stride, t->data);
    sceGuTexFlush();
    texIsAtlas = 0;
    filter(1);
    /* 64-texel wide strips keep the texture cache happy. UVs are inset by
       half a texel: the bilinear filter must never read the texel past the
       right/bottom edge, which is the next row (or random VRAM) because the
       buffer is narrower than the power-of-two texture - that was the
       "glitchy picture" edge. */
    int strips = (t->w + 63) / 64;
    V *v = verts(strips * 2);
    float uMax = t->w - 0.5f, vMax = t->h - 0.5f;
    float sx = w / t->w;
    for (int i = 0; i < strips; i++) {
        float p0 = i * 64.0f, p1 = p0 + 64.0f;
        if (p1 > t->w) p1 = t->w;
        float u0 = p0 < 0.5f ? 0.5f : p0, u1 = p1 > uMax ? uMax : p1;
        sv(&v[i * 2], u0, 0.5f, tint, x + p0 * sx, y);
        sv(&v[i * 2 + 1], u1, vMax, tint, x + p1 * sx, y + h);
    }
    sceGuDrawArray(GU_SPRITES, VFMT, strips * 2, 0, v);
}

/* ---------------------------------------------------------------- text */
static u32 utf8next(const char **ps) {
    const u8 *s = (const u8 *)*ps;
    u32 c = *s++;
    if (c >= 0xF0 && s[0] && s[1] && s[2]) { c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); s += 3; }
    else if (c >= 0xE0 && s[0] && s[1]) { c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63); s += 2; }
    else if (c >= 0xC0 && s[0]) { c = ((c & 31) << 6) | (s[0] & 63); s += 1; }
    *ps = (const char *)s;
    return c;
}

static const GlyphInfo *glyph(int font, u32 cp) {
    int idx = -1;
    if (cp >= 32 && cp < 127) idx = cp - 32;
    else if (cp >= 0x410 && cp < 0x450) idx = 95 + (cp - 0x410);
    else for (int i = 159; i < NGLYPHS; i++) if (glyph_cps[i] == cp) { idx = i; break; }
    if (idx < 0) idx = '?' - 32;
    const GlyphInfo *g = &font_glyphs[font][idx];
    if (!g->w && !g->adv) g = &font_glyphs[font]['?' - 32];
    return g;
}

int gfxLineH(int font) { return font_metrics[font].ascent + font_metrics[font].descent; }

static float textRun(int font, float x, float y, float sc, Color c, const char *s, int draw) {
    float base = y + font_metrics[font].ascent * sc;
    float pen = x;
    V *v = NULL;
    int k = 0;
    if (draw) {
        int n = 0;
        for (const char *p = s; *p;) { utf8next(&p); n++; }
        if (!n) return 0;
        v = verts(n * 2);
    }
    int crisp = (sc == 1.0f);
    for (const char *p = s; *p;) {
        const GlyphInfo *g = glyph(font, utf8next(&p));
        if (draw && g->w) {
            float gx = pen + g->ox * sc, gy = base + g->oy * sc;
            if (crisp) { gx = floorf(gx + 0.5f); gy = floorf(gy + 0.5f); }
            sv(&v[k++], g->x, g->y, c, gx, gy);
            sv(&v[k++], g->x + g->w, g->y + g->h, c, gx + g->w * sc, gy + g->h * sc);
        }
        pen += g->adv * sc;
    }
    if (draw && k) {
        useAtlas();
        filter(!crisp);
        sceGuDrawArray(GU_SPRITES, VFMT, k, 0, v);
    }
    return pen - x;
}

float gfxTextW(int font, const char *s) { return textRun(font, 0, 0, 1, 0, s, 0); }
void gfxText(int font, float x, float y, Color c, const char *s) {
    if (colA(c)) textRun(font, x, y, 1, c, s, 1);
}
void gfxTextC(int font, float cx, float y, Color c, const char *s) {
    gfxText(font, floorf(cx - gfxTextW(font, s) * 0.5f), y, c, s);
}
void gfxTextR(int font, float rx, float y, Color c, const char *s) {
    gfxText(font, rx - gfxTextW(font, s), y, c, s);
}
void gfxTextScaled(int font, float cx, float cy, float sc, Color c, const char *s) {
    if (!colA(c)) return;
    float w = gfxTextW(font, s) * sc, h = gfxLineH(font) * sc;
    if (sc > 0.995f && sc < 1.005f) { gfxText(font, floorf(cx - w / 2), floorf(cy - h / 2), c, s); return; }
    textRun(font, cx - w / 2, cy - h / 2, sc, c, s, 1);
}

void gfxTextFit(int font, float x, float y, float maxW, Color c, const char *s) {
    if (gfxTextW(font, s) <= maxW) { gfxText(font, x, y, c, s); return; }
    char buf[160];
    float ell = gfxTextW(font, "\xE2\x80\xA6");
    const char *p = s, *last = s;
    float w = 0;
    while (*p) {
        const char *q = p;
        const GlyphInfo *g = glyph(font, utf8next(&q));
        if (w + g->adv + ell > maxW) break;
        w += g->adv;
        p = q;
        last = p;
    }
    int n = last - s;
    if (n > (int)sizeof(buf) - 4) n = sizeof(buf) - 4;
    memcpy(buf, s, n);
    while (n > 0 && buf[n - 1] == ' ') n--;
    memcpy(buf + n, "\xE2\x80\xA6", 4);
    gfxText(font, x, y, c, buf);
}
