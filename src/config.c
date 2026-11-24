/* Config (settings.cfg) and themes: presets, custom theme derivation, animation. */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "audio.h"
#include "library.h"
#include "version.h"

Config cfg;
char appDir[256] = "ms0:/PSP/GAME/PS5SHELL";

/* ================================================================ themes */
const Theme presets[NPRESETS] = {
    { /* black */
        RGB(22, 22, 26), RGB(0, 0, 0), WHITE, RGBA(255, 255, 255, 170), RGBA(255, 255, 255, 105),
        RGBA(255, 255, 255, 20), RGBA(255, 255, 255, 38), RGB(24, 24, 28), WHITE, RGB(10, 10, 12),
        WHITE, RGBA(255, 255, 255, 70), RGBA(0, 0, 0, 170), RGBA(0, 0, 0, 170), RGB(235, 40, 60), 1.0f},
    { /* white */
        RGB(252, 252, 253), RGB(228, 229, 234), RGB(16, 16, 20), RGBA(16, 16, 20, 165), RGBA(16, 16, 20, 105),
        RGBA(0, 0, 0, 12), RGBA(0, 0, 0, 24), RGB(255, 255, 255), RGB(16, 16, 20), WHITE,
        RGB(16, 16, 20), RGBA(0, 0, 0, 40), RGBA(255, 255, 255, 150), RGBA(0, 0, 0, 60), RGB(220, 30, 50), 0.35f},
    { /* black + red */
        RGB(46, 6, 10), RGB(0, 0, 0), WHITE, RGBA(255, 255, 255, 170), RGBA(255, 255, 255, 105),
        RGBA(255, 255, 255, 20), RGBA(255, 255, 255, 38), RGB(28, 14, 16), RGB(230, 22, 48), WHITE,
        RGB(240, 40, 60), RGBA(255, 60, 80, 90), RGBA(0, 0, 0, 170), RGBA(0, 0, 0, 170), RGB(240, 40, 60), 0.5f},
    { /* white + red */
        WHITE, RGB(242, 234, 235), RGB(20, 14, 16), RGBA(20, 14, 16, 165), RGBA(20, 14, 16, 105),
        RGBA(0, 0, 0, 12), RGBA(0, 0, 0, 24), WHITE, RGB(220, 18, 42), WHITE,
        RGB(220, 18, 42), RGBA(220, 18, 42, 50), RGBA(255, 255, 255, 150), RGBA(60, 0, 0, 60), RGB(220, 18, 42), 0.25f},
};
const char *const themeNames[NPRESETS + 1] = {"Чёрная", "Белая", "Чёрно-красная", "Бело-красная", "Своя"};

static int luma(Color c) { return ((c & 255) * 299 + ((c >> 8) & 255) * 587 + ((c >> 16) & 255) * 114) / 1000; }
Color lumaText(Color bg) { return luma(bg) > 150 ? RGB(14, 14, 18) : WHITE; }
int colLuma(Color c) { return luma(c); }

void themeBase(int theme, Color out[5]) {
    if (theme >= THEME_CUSTOM) {
        out[0] = cfg.cBgTop; out[1] = cfg.cBgBot; out[2] = cfg.cAccent; out[3] = cfg.cText; out[4] = cfg.cFocus;
    } else {
        const Theme *p = &presets[theme];
        out[0] = p->bgTop; out[1] = p->bgBot; out[2] = p->accent; out[3] = p->text; out[4] = p->focus;
    }
}

/* a full theme from 5 user colors */
static void deriveTheme(Theme *t) {
    Color b[5];
    themeBase(THEME_CUSTOM, b);
    int dark = luma(colLerp(b[0], b[1], 0.5f)) < 128;
    t->bgTop = b[0]; t->bgBot = b[1]; t->accent = b[2]; t->text = b[3]; t->focus = b[4];
    t->text2 = colWithA(b[3], 165);
    t->text3 = colWithA(b[3], 105);
    t->surface = colWithA(b[3], dark ? 22 : 14);
    t->surfaceHi = colWithA(b[3], dark ? 40 : 26);
    t->sheet = dark ? colLerp(b[1], b[3], 0.09f) : colLerp(b[0], WHITE, 0.7f);
    t->sheet |= 0xFF000000;
    t->onAccent = lumaText(b[2]);
    t->particle = colWithA(b[2] == b[3] ? b[3] : b[2], dark ? 80 : 50);
    t->scrim = colWithA(b[1], dark ? 170 : 150);
    t->shadow = dark ? RGBA(0, 0, 0, 170) : RGBA(0, 0, 0, 60);
    t->danger = RGB(230, 30, 50);
    t->tint = dark ? 1.0f : 0.35f;
}

Theme T;
static Theme target;

void themeStep(void) {
    if (cfg.theme >= THEME_CUSTOM) deriveTheme(&target);
    else target = presets[cfg.theme];
    const float k = 0.12f;
#define TL(f) T.f = colLerp(T.f, target.f, clamp01(k * animMul))
    TL(bgTop); TL(bgBot); TL(text); TL(text2); TL(text3); TL(surface); TL(surfaceHi); TL(sheet);
    TL(accent); TL(onAccent); TL(focus); TL(particle); TL(scrim); TL(shadow); TL(danger);
#undef TL
    T.tint = approach(T.tint, target.tint * cfg.tint / 100.0f, k);
}

/* ================================================================ config */
float animMul = 1.0f;

void cfgDefaults(void) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.theme = 0;
    cfg.cBgTop = RGB(18, 24, 44); cfg.cBgBot = RGB(2, 4, 10);
    cfg.cAccent = RGB(0, 112, 220); cfg.cText = WHITE; cfg.cFocus = WHITE;
    cfg.wallDim = 40; cfg.wallBlur = 0; cfg.gameArt = 1; cfg.tint = 40;
    cfg.particles = 2; cfg.tileSize = 1; cfg.radius = 10; cfg.animSpeed = 1; cfg.hints = 1;
    cfg.sfx = 1; cfg.sfxVol = 80; cfg.music = 1; cfg.musicVol = 60;
    cfg.clock24 = 1; cfg.startTab = 0;
}

static const struct { const char *k; int *p; int lo, hi; } intKeys[] = {
    {"theme", &cfg.theme, 0, THEME_CUSTOM}, {"wall_dim", &cfg.wallDim, 0, 90}, {"wall_blur", &cfg.wallBlur, 0, 3},
    {"game_art", &cfg.gameArt, 0, 1}, {"tint", &cfg.tint, 0, 100}, {"particles", &cfg.particles, 0, 3},
    {"tile_size", &cfg.tileSize, 0, 2}, {"radius", &cfg.radius, 0, 20}, {"anim", &cfg.animSpeed, 0, 3},
    {"hints", &cfg.hints, 0, 1}, {"sfx", &cfg.sfx, 0, 1}, {"sfx_vol", &cfg.sfxVol, 0, 100},
    {"music", &cfg.music, 0, 1}, {"music_vol", &cfg.musicVol, 0, 100}, {"clock24", &cfg.clock24, 0, 1},
    {"start_tab", &cfg.startTab, 0, 1},
};
static const struct { const char *k; Color *p; } colKeys[] = {
    {"bg_top", &cfg.cBgTop}, {"bg_bottom", &cfg.cBgBot}, {"accent", &cfg.cAccent},
    {"text", &cfg.cText}, {"focus", &cfg.cFocus},
};
#define NINT (int)(sizeof(intKeys) / sizeof(intKeys[0]))
#define NCOL (int)(sizeof(colKeys) / sizeof(colKeys[0]))

static void cfgPath(char *out, int max) { snprintf(out, max, "%s/settings.cfg", appDir); }

void cfgLoad(void) {
    char path[300];
    cfgPath(path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    static char buf[2048];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *l = strtok(buf, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        for (int i = 0; i < NINT; i++)
            if (!strcmp(l, intKeys[i].k)) {
                int x = atoi(v);
                if (x < intKeys[i].lo) x = intKeys[i].lo;
                if (x > intKeys[i].hi) x = intKeys[i].hi;
                *intKeys[i].p = x;
            }
        for (int i = 0; i < NCOL; i++)
            if (!strcmp(l, colKeys[i].k)) {
                u32 rgb = (u32)strtoul(v, NULL, 16); /* RRGGBB */
                *colKeys[i].p = RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
            }
        if (!strcmp(l, "wallpaper")) snprintf(cfg.wallpaper, sizeof(cfg.wallpaper), "%s", v);
    }
}

void cfgSave(void) {
    char path[300];
    static char buf[2048];
    int n = snprintf(buf, sizeof(buf), "# " SYS_FULL " settings\n");
    cfgPath(path, sizeof(path));
    for (int i = 0; i < NINT; i++) n += snprintf(buf + n, sizeof(buf) - n, "%s=%d\n", intKeys[i].k, *intKeys[i].p);
    for (int i = 0; i < NCOL; i++) {
        Color c = *colKeys[i].p;
        n += snprintf(buf + n, sizeof(buf) - n, "%s=%02X%02X%02X\n", colKeys[i].k,
                      (unsigned)(c & 255), (unsigned)((c >> 8) & 255), (unsigned)((c >> 16) & 255));
    }
    n += snprintf(buf + n, sizeof(buf) - n, "wallpaper=%s\n", cfg.wallpaper);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

void cfgApply(void) {
    static const float speeds[4] = {0.55f, 1.0f, 1.8f, 100.0f};
    static char lastWall[192] = "\x01";
    static int lastBlur = -1;
    animMul = speeds[cfg.animSpeed & 3];
    audioSetSfx(cfg.sfx);
    audioSetMusic(cfg.music);
    audioSetVolume(cfg.sfxVol / 100.0f, cfg.musicVol / 100.0f);
    if (strcmp(lastWall, cfg.wallpaper) || lastBlur != cfg.wallBlur) {
        snprintf(lastWall, sizeof(lastWall), "%s", cfg.wallpaper);
        lastBlur = cfg.wallBlur;
        libRequestWallpaper(cfg.wallpaper, cfg.wallBlur);
    }
}

