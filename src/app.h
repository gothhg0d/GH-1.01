/* Shared app state: config, themes, input, animation helpers. */
#pragma once
#include <math.h>
#include <pspctrl.h>
#include "gfx.h"

#define WHITE RGB(255, 255, 255)
#define BLACK RGB(0, 0, 0)

typedef struct {
    Color bgTop, bgBot;      /* background gradient                 */
    Color text, text2, text3;/* primary / secondary / hint text     */
    Color surface, surfaceHi;/* translucent cards                   */
    Color sheet;             /* opaque panels                       */
    Color accent, onAccent;  /* Play button, toggles                */
    Color focus;             /* focus ring                          */
    Color particle, scrim, shadow;
    Color danger;
    float tint;              /* how much the game color tints the bg*/
} Theme;

#define NPRESETS 4
#define THEME_CUSTOM NPRESETS
extern Theme T;                         /* animated, current */
extern const Theme presets[NPRESETS];
extern const char *const themeNames[NPRESETS + 1];

/* everything the user can customize; saved to settings.cfg */
typedef struct {
    int theme;                      /* 0..3 presets, 4 = custom          */
    Color cBgTop, cBgBot, cAccent, cText, cFocus; /* custom theme colors */
    char wallpaper[192];            /* "" = none                         */
    int wallDim;                    /* 0..90 %                           */
    int wallBlur;                   /* 0..3                              */
    int gameArt;                    /* show PIC1 behind the selected game*/
    int tint;                       /* 0..100 % tint by game color       */
    int particles;                  /* 0..3                              */
    int tileSize;                   /* 0..2                              */
    int radius;                     /* 0..20 px (at focused size)        */
    int animSpeed;                  /* 0 slow, 1 normal, 2 fast, 3 off   */
    int hints;
    int sfx, sfxVol, music, musicVol; /* volumes 0..100                  */
    int clock24;
    int startTab;
} Config;

extern Config cfg;
extern char appDir[256];
void cfgDefaults(void);
void cfgLoad(void);
void cfgSave(void);
void cfgApply(void);                /* audio + wallpaper after a change   */
void themeStep(void);
void themeBase(int theme, Color out[5]); /* bgTop, bgBot, accent, text, focus */

extern volatile int appRunning;
extern int frameCount;
extern float animMul;               /* from cfg.animSpeed */

/* input: stick is merged into the d-pad */
extern u32 padNow, padPrev;
extern int padLx, padLy;
void pollPad(void);
static inline int pressed(u32 b) { return (padNow & b) && !(padPrev & b); }
int repeatBtn(u32 b);
int heldFrames(u32 b);

/* animation helpers (frame based, 60 fps, scaled by the animation speed) */
static inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static inline float approach(float v, float t, float k) {
    k *= animMul;
    if (k > 1) k = 1;
    return v + (t - v) * k;
}
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float easeOutCubic(float t) { t = 1 - clamp01(t); return 1 - t * t * t; }
static inline float easeInOutCubic(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4 * t * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * (-2 * t + 2) / 2;
}

/* shared drawing (main.c) */
void drawBackdrop(Color tint, float particles);
float drawHint(float x, float y, const char *glyph, const char *label, float alpha);
void beginFrame(void);
void endFrame(void);
void toast(const char *m);
Color lumaText(Color bg);           /* black or white, readable on bg */
int colLuma(Color c);
void requestRescan(void);           /* handled by the home loop */

/* settings screen (settings.c) */
void settingsOpen(void);
int  settingsUpdate(void);          /* 0 when the screen was closed */
void settingsDraw(float a);
