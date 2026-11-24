/*
 * GH-1.01 - minimal PS5-style system shell for PSP 3000/3008 (runs on ARK-4).
 *
 * Home:  Left/Right choose, X play, Triangle info, L/R tabs,
 *        SELECT control center, START settings.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspiofilemgr.h>
#include <psploadexec_kernel.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "audio.h"
#include "library.h"
#include "version.h"

PSP_MODULE_INFO("GH-1.01", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

/* ================================================================ ARK-4 SystemControl
 * Values and call order follow ARK-4's own launcher (extras/menus/arkMenu/src/iso.cpp,
 * eboot.cpp; common/include/systemctrl_se.h). Link with libpspsystemctrl_user.a. */
int  sctrlKernelLoadExecVSHWithApitype(int apitype, const char *file, struct SceKernelLoadExecVSHParam *param);
void sctrlSESetUmdFile(char *file);
void sctrlSESetBootConfFileIndex(int index);
void sctrlSESetDiscType(int type);

#define MODE_UMD          0
#define MODE_INFERNO      3       /* ISO driver */
#define DISC_TYPE_GAME    0x10
#define RL_ISO            0x123
#define RL_ISO_GO         0x125
#define RL_ISO_PBOOT      0x124
#define RL_ISO_PBOOT_GO   0x126
#define RL_HOMEBREW       0x141
#define RL_POPS           0x144
#define RL_POPS_GO        0x155
#define UMD_EBOOT_BIN     "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"
#define UMD_EBOOT_OLD     "disc0:/PSP_GAME/SYSDIR/EBOOT.OLD"

volatile int appRunning = 1;
int frameCount = 0;

/* ================================================================ input */
u32 padNow, padPrev;
int padLx = 128, padLy = 128;
static int held[32];

void pollPad(void) {
    SceCtrlData c;
    sceCtrlPeekBufferPositive(&c, 1);
    padPrev = padNow;
    padNow = c.Buttons;
    padLx = c.Lx;
    padLy = c.Ly;
    if (padLx < 40) padNow |= PSP_CTRL_LEFT;
    if (padLx > 215) padNow |= PSP_CTRL_RIGHT;
    if (padLy < 40) padNow |= PSP_CTRL_UP;
    if (padLy > 215) padNow |= PSP_CTRL_DOWN;
    /* L + R + SELECT: performance overlay (the SELECT press is swallowed) */
    if ((padNow & PSP_CTRL_LTRIGGER) && (padNow & PSP_CTRL_RTRIGGER) && (padNow & PSP_CTRL_SELECT) && !(padPrev & PSP_CTRL_SELECT)) {
        extern void togglePerf(void);
        togglePerf();
        padNow &= ~(PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER);
    }
    for (int i = 0; i < 32; i++) {
        if (padNow & (1u << i)) { if (held[i] < 100000) held[i]++; }
        else held[i] = 0;
    }
}
int heldFrames(u32 b) { return held[__builtin_ctz(b)]; }
int repeatBtn(u32 b) {
    int h = held[__builtin_ctz(b)];
    return h == 1 || (h > 16 && (h % 4) == 0);
}

/* ================================================================ callbacks */
static int exitCb(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    appRunning = 0;
    sceKernelExitGame();
    return 0;
}
static int cbThread(SceSize a, void *p) {
    (void)a; (void)p;
    int id = sceKernelCreateCallback("ExitCb", exitCb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

/* ================================================================ shared drawing */
#define MAXP 60
typedef struct { float x, y, v, s, a, ph; } Particle;
static Particle parts[MAXP];
static float partVis[MAXP];
static float wallA = 0;
static int wallSerialSeen = -1;

static void particlesInit(void) {
    for (int i = 0; i < MAXP; i++) {
        parts[i].x = rand() % SCR_W;
        parts[i].y = rand() % SCR_H;
        parts[i].v = 0.12f + (rand() % 100) / 260.0f;
        parts[i].s = 1.0f + (rand() % 100) / 60.0f;
        parts[i].a = 0.3f + (rand() % 100) / 140.0f;
        parts[i].ph = (rand() % 628) / 100.0f;
    }
}

/* Background. Fill-rate is the main cost on the PSP GPU, so every full-screen
   layer that would be invisible is skipped:
   - the gradient when an opaque wallpaper fully covers it (the frame is already
     cleared to bgBot),
   - the dim layer on dark themes: the wallpaper is darkened by vertex color
     (one textured pass instead of two),
   - glows that would be (almost) transparent.
   Particles are one batched draw call. */
void drawBackdrop(Color tint, float particles) {
    Texture wt;
    int hasWall = libWallpaper(&wt);
    if (libWallpaperSerial() != wallSerialSeen) { wallSerialSeen = libWallpaperSerial(); wallA = 0.2f; }
    wallA = approach(wallA, hasWall ? 1.0f : 0.0f, 0.08f);

    if (!(hasWall && wallA > 0.995f)) {
        Color top = colLerp(T.bgTop, tint, T.tint * 0.6f);
        gfxGradV(0, 0, SCR_W, SCR_H, top, T.bgBot);
    }
    if (hasWall && wallA > 0.01f) {
        float keep = 1.0f - cfg.wallDim / 100.0f;
        if (colLuma(T.bgBot) < 48) {
            int g = (int)(255 * keep);
            gfxImage(&wt, 0, 0, SCR_W, SCR_H, colFade(RGB(g, g, g), wallA));
        } else {
            gfxImage(&wt, 0, 0, SCR_W, SCR_H, colFade(WHITE, wallA));
            if (cfg.wallDim) gfxRect(0, 0, SCR_W, SCR_H, colFade(colWithA(T.bgBot, cfg.wallDim * 255 / 100), wallA));
        }
        int ta = (int)(60 * T.tint * wallA);
        if (ta > 3) gfxGradV(0, 0, SCR_W, 136, colWithA(tint, ta), colWithA(tint, 0));
    }

    float t = frameCount * 0.004f;
    float gl = 1 - wallA * 0.7f;
    int g1 = (int)(90 * T.tint * gl), g2 = (int)(14 * gl);
    if (g1 > 3) gfxGlow(390 + cosf(t) * 40, 30 + sinf(t * 1.3f) * 18, 230, colWithA(colLerp(T.bgTop, tint, 0.6f), g1));
    if (g2 > 3) gfxGlow(70 + sinf(t * 0.7f) * 50, 250, 200, colWithA(T.accent, g2));

    static const int counts[4] = {0, 14, 32, MAXP};
    static float px[MAXP], py[MAXP], pr[MAXP];
    static Color pc[MAXP];
    int n = counts[cfg.particles & 3], k = 0;
    for (int i = 0; i < MAXP; i++) {
        partVis[i] = approach(partVis[i], i < n ? particles : 0, 0.04f);
        if (partVis[i] < 0.01f) continue;
        Particle *p = &parts[i];
        p->y -= p->v;
        p->x += sinf(p->y * 0.02f + p->ph) * 0.12f;
        if (p->y < -4) { p->y = SCR_H + 4; p->x = rand() % SCR_W; }
        float tw = 0.6f + 0.4f * sinf(frameCount * 0.05f + p->ph);
        px[k] = p->x; py[k] = p->y; pr[k] = p->s;
        pc[k++] = colFade(T.particle, p->a * tw * partVis[i]);
    }
    gfxCircles(px, py, pr, pc, k);
}

float drawHint(float x, float y, const char *glyph, const char *label, float a) {
    if (glyph) {
        gfxText(F_BODY, x, y - 1, colFade(T.text2, a), glyph);
        x += gfxTextW(F_BODY, glyph) + 4;
    }
    gfxText(F_SMALL, x, y, colFade(T.text3, a), label);
    return x + gfxTextW(F_SMALL, label) + 14;
}

/* first character of a word, upper-cased (Latin + Cyrillic) */
static void firstLetter(const char *s, char *out) {
    u8 c = (u8)s[0];
    if (!c) { strcpy(out, "?"); return; }
    if (c < 0x80) { out[0] = (c >= 'a' && c <= 'z') ? c - 32 : c; out[1] = 0; return; }
    if (c == 0xD0 && (u8)s[1] >= 0xB0) { out[0] = 0xD0; out[1] = (u8)s[1] - 0x20; out[2] = 0; return; }
    if (c == 0xD1 && (u8)s[1] < 0x90) { out[0] = 0xD0; out[1] = (u8)s[1] + 0x20; out[2] = 0; return; }
    int len = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2);
    memcpy(out, s, len);
    out[len] = 0;
}

/* Frame timing. The performance overlay (hold L+R and press SELECT) shows the
   real cost on the console: CPU+GPU time of the frame, fps and free RAM. */
static int perfOn = 0;
static u32 tFrameStart, tLastSwap;
static float perfWork = 0, perfFps = 60;
void togglePerf(void) { perfOn = !perfOn; }

void beginFrame(void) {
    tFrameStart = sceKernelGetSystemTimeLow();
    themeStep();
    gfxBeginFrame(T.bgBot);
}

void endFrame(void) {
    if (perfOn) {
        char b[96];
        snprintf(b, sizeof(b), "кадр %.1f мс • %.0f fps • своб. %.1f МБ", perfWork, perfFps,
                 sceKernelTotalFreeMemSize() / 1048576.0f);
        float w = gfxTextW(F_SMALL, b) + 16;
        gfxRound(SCR_W - w - 6, SCR_H - 22, w, 18, 9, RGBA(0, 0, 0, 170));
        gfxText(F_SMALL, SCR_W - w + 2, SCR_H - 19, perfWork > 15 ? RGB(255, 90, 90) : RGB(120, 255, 150), b);
    }
    gfxFinish();                                   /* CPU list building + GPU drawing done */
    u32 now = sceKernelGetSystemTimeLow();
    perfWork = perfWork * 0.9f + ((now - tFrameStart) / 1000.0f) * 0.1f;
    gfxSwap();                                     /* waits for vblank */
    now = sceKernelGetSystemTimeLow();
    if (tLastSwap) perfFps = perfFps * 0.9f + (1e6f / (now - tLastSwap)) * 0.1f;
    tLastSwap = now;
    frameCount++;
}

/* power/clock syscalls are cached: no need to ask the hardware 60 times a second */
static int batteryPct(void) {
    static int pct = -1, at = -1000;
    if (frameCount - at >= 120) { pct = scePowerGetBatteryLifePercent(); at = frameCount; }
    return pct;
}
static int batteryCharging(void) {
    static int ch = 0, at = -1000;
    if (frameCount - at >= 120) { ch = scePowerIsBatteryCharging(); at = frameCount; }
    return ch;
}
static void nowTime(ScePspDateTime *out) {
    static ScePspDateTime t;
    static int at = -1000;
    if (frameCount - at >= 30) { sceRtcGetCurrentClockLocalTime(&t); at = frameCount; }
    *out = t;
}

static void timeStr(char *buf, int max) {
    ScePspDateTime t;
    nowTime(&t);
    if (cfg.clock24) snprintf(buf, max, "%d:%02d", t.hour, t.minute);
    else snprintf(buf, max, "%d:%02d %s", (t.hour + 11) % 12 + 1, t.minute, t.hour < 12 ? "AM" : "PM");
}

/* ================================================================ home state */
#define GAP  10.0f
#define ROW_Y 44.0f
#define LEFT 18.0f
static const float tileSizes[3][2] = {{42, 62}, {50, 74}, {58, 88}};
#define TILE_S (tileSizes[cfg.tileSize][0])
#define TILE_F (tileSizes[cfg.tileSize][1])

enum { OV_NONE, OV_INFO, OV_CC, OV_SETTINGS };

static int tab = 0, sel[2];
static float cam[2], tileFocus[2][MAXG];
static int introFrame = -1000;
static float tabA[2] = {1, 0}, slideX = 0, contentA = 1;
static float titleAnim = 1;
static int lastSel = -1, lastTab = -1, settle = 0;
static float selX, selY, selS;
static float playPress = 0;

static int overlay = OV_NONE;
static float ovInfo, ovCC, ovSet;
static int ccSel = 0;
static float ccFocusX = 0;

static char toastMsg[96];
static int toastT = 0;
static float toastA = 0;

static Color tintCur = RGB(20, 20, 30);

static int artShown = -1;
static Game *artShownGame = NULL;
static float artShownA = 0, artFadeA = 0;
static int artFading = 0;

void toast(const char *m) {
    snprintf(toastMsg, sizeof(toastMsg), "%s", m);
    toastT = 170;
}

static void resetArt(void) {
    artShown = -1;
    artShownGame = NULL;
    artShownA = artFadeA = 0;
    artFading = 0;
}

static Game *curGame(void) { return ngames[tab] ? &games[tab][sel[tab]] : NULL; }

static void initials(const char *s, char *out) {
    int n = 0, words = 0, newWord = 1;
    while (*s && words < 2) {
        u8 c = (u8)*s;
        int len = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2));
        int alnum = c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum && newWord) {
            char l[8];
            firstLetter(s, l);
            for (int i = 0; l[i]; i++) out[n++] = l[i];
            words++;
            newWord = 0;
        } else if (!alnum) newWord = 1;
        for (int i = 0; i < len && *s; i++) s++;
    }
    if (!n) out[n++] = '?';
    out[n] = 0;
}

static void metaStr(const Game *g, char *buf, int max) {
    char kind[24];
    switch (g->type) {
    case T_ISO: snprintf(kind, sizeof(kind), "PSP • %s", g->fmt); break;
    case T_PSN: snprintf(kind, sizeof(kind), "PSP • PSN"); break;
    case T_PS1: snprintf(kind, sizeof(kind), "PS1"); break;
    default:    snprintf(kind, sizeof(kind), "Приложение"); break;
    }
    if (g->sizeMB >= 1024)
        snprintf(buf, max, "%s • %lu,%lu ГБ", kind, (unsigned long)(g->sizeMB / 1024), (unsigned long)((g->sizeMB % 1024) * 10 / 1024));
    else snprintf(buf, max, "%s • %lu МБ", kind, (unsigned long)(g->sizeMB ? g->sizeMB : 1));
}

/* ================================================================ tiles */
static void drawIconFit(Game *g, float x, float y, float s, float a) {
    float sc = s / g->icon.w;
    if (g->icon.h * sc > s) sc = s / g->icon.h;
    float w = g->icon.w * sc, h = g->icon.h * sc;
    gfxImage(&g->icon, x + (s - w) / 2, y + (s - h) / 2, w, h, colFade(WHITE, a));
}

static void drawTile(Game *g, float x, float y, float s, float focus, float a) {
    if (x > SCR_W + 10 || x + s < -10 || a <= 0.01f) return;
    float r = cfg.radius * s / TILE_F;
    gfxShadow(x, y + s * 0.06f, s, s, 5 + focus * 5, colFade(T.shadow, a * (0.55f + 0.45f * focus)));
    Color base = g->iconState == ICON_READY ? g->iconAvg : g->color;
    gfxRoundGradV(x, y, s, s, r, colFade(colLerp(base, WHITE, 0.06f), a), colFade(colLerp(base, BLACK, 0.35f), a));
    if (g->iconState == ICON_READY) g->iconFade = approach(g->iconFade, 1, 0.12f);
    if (g->iconFade < 0.99f) {
        char ini[12];
        initials(g->name, ini);
        int f = s > 62 ? F_TITLE : F_LABEL;
        gfxTextC(f, x + s / 2, y + (s - gfxLineH(f)) / 2, colFade(RGBA(255, 255, 255, 230), a * (1 - g->iconFade)), ini);
    }
    if (g->iconFade > 0.01f) drawIconFit(g, x, y, s, a * g->iconFade);
    if (focus > 0.02f) gfxRing(x - 3.5f, y - 3.5f, s + 7, s + 7, r + 4, colFade(T.focus, a * focus));
}

/* ================================================================ background art */
static void artUpdate(void) {
    Game *cur = curGame();
    Game *want = (cfg.gameArt && settle > 14 && cur && !cur->noArt) ? cur : NULL;
    artShownA = approach(artShownA, (cfg.gameArt && artShownGame && artShownGame == cur) ? 1.0f : 0.0f, 0.07f);
    if (artFading) {
        artFadeA += 0.05f * animMul;
        if (artFadeA >= 1) {
            artShown = artTarget;
            artShownGame = artGame;
            artShownA = 1;
            artFadeA = 0;
            artFading = 0;
            artState = ART_IDLE;
        }
    } else if (artState == ART_READY) {
        if (artGame == cur && cfg.gameArt) { artFading = 1; artFadeA = 0; }
        else artState = ART_IDLE;
    } else if (artState == ART_FAILED) {
        artState = ART_IDLE;
    } else if (artState == ART_IDLE && want && want != artShownGame) {
        libRequestArt(want, artShown == 0 ? 1 : 0);
    }
}

static void artDraw(void) {
    float vis = artShownA > artFadeA ? artShownA : artFadeA;
    if (vis < 0.01f) return;
    Texture tex;
    if (artShown >= 0 && artShownA > 0.01f) {
        gfxArtSlot(artShown, &tex);
        gfxImage(&tex, 0, 0, SCR_W, SCR_H, colFade(WHITE, artShownA));
    }
    if (artFading) {
        gfxArtSlot(artTarget, &tex);
        gfxImage(&tex, 0, 0, SCR_W, SCR_H, colFade(WHITE, easeInOutCubic(artFadeA)));
    }
    gfxGradH(0, 0, 320, SCR_H, colFade(colWithA(T.bgBot, 215), vis), colFade(colWithA(T.bgBot, 0), vis));
    gfxGradV(0, 110, SCR_W, 162, colFade(colWithA(T.bgBot, 0), vis), colFade(colWithA(T.bgBot, 235), vis));
    gfxGradV(0, 0, SCR_W, 40, colFade(colWithA(T.bgBot, 150), vis), colFade(colWithA(T.bgBot, 0), vis));
}

/* ================================================================ top bar */
static void drawTopBar(float a) {
    static const char *tabs[2] = {"Игры", "Приложения"};
    float x = LEFT;
    for (int i = 0; i < 2; i++) {
        tabA[i] = approach(tabA[i], i == tab ? 1.0f : 0.0f, 0.18f);
        gfxText(F_LABEL, x, 10, colFade(colLerp(T.text3, T.text, tabA[i]), a), tabs[i]);
        float w = gfxTextW(F_LABEL, tabs[i]);
        gfxRound(x + w / 2 - 8 * tabA[i], 28, 16 * tabA[i], 2, 1, colFade(T.text, a * tabA[i]));
        x += w + 20;
    }
    char buf[16];
    timeStr(buf, sizeof(buf));
    gfxTextR(F_LABEL, SCR_W - LEFT, 10, colFade(T.text, a), buf);
    float bx = SCR_W - LEFT - gfxTextW(F_LABEL, buf) - 30;
    int pct = batteryPct();
    gfxRing(bx, 13, 20, 11, 3.5f, colFade(T.text2, a));
    gfxRound(bx + 20.5f, 16, 2, 5, 1, colFade(T.text2, a));
    if (pct >= 0) {
        Color bc = pct <= 15 ? T.danger : T.text;
        if (batteryCharging()) bc = colLerp(T.text, RGB(60, 210, 110), 0.5f + 0.5f * sinf(frameCount * 0.08f));
        gfxRound(bx + 2.5f, 15.5f, 15 * pct / 100.0f, 6, 1.5f, colFade(bc, a));
    }
}

/* ================================================================ home */
static void drawHome(float a) {
    Game *list = games[tab];
    int n = ngames[tab];
    int s = sel[tab];
    Game *g = curGame();

    Color tgt = g ? (g->iconState == ICON_READY ? g->iconAvg : g->color) : T.bgTop;
    tintCur = colLerp(tintCur, tgt, clamp01(0.06f * animMul));
    /* an opaque PIC1 covering the screen hides the whole background: skip it */
    if (!(artShown >= 0 && artShownA > 0.995f && !artFading)) drawBackdrop(tintCur, 1);
    artDraw();
    drawTopBar(a);

    slideX = approach(slideX, 0, 0.16f);
    contentA = approach(contentA, 1, 0.12f);
    float ca = a * contentA;

    if (n == 0) {
        gfxTextC(F_TITLE, 240 + slideX, 112, colFade(T.text, ca), "Здесь пока пусто");
        gfxTextC(F_SMALL, 240 + slideX, 142, colFade(T.text2, ca),
                 tab == 0 ? "Положите ISO/CSO/ZSO в ms0:/ISO, PS1/PSN — в ms0:/PSP/GAME"
                          : "Положите homebrew в ms0:/PSP/GAME/<папка>/EBOOT.PBP");
    } else {
        cam[tab] = approach(cam[tab], s * (TILE_S + GAP), 0.2f);
        float x = LEFT - cam[tab] + slideX;
        float introT = (frameCount - introFrame) * (animMul > 10 ? 100.0f : animMul);
        for (int i = 0; i < n; i++) {
            tileFocus[tab][i] = approach(tileFocus[tab][i], i == s ? 1.0f : 0.0f, 0.22f);
            float f = easeOutCubic(tileFocus[tab][i]);
            float size = lerpf(TILE_S, TILE_F, f);
            if (x > SCR_W) break;
            int rel = i - s + 2;
            float ia = easeOutCubic(clamp01((introT - rel * 3) / 16.0f));
            float ty = ROW_Y + (1 - ia) * 14;
            if (x + size > -20) drawTile(&list[i], x + (1 - ia) * 24, ty, size, f, ca * ia * (i < s ? 0.5f : 1));
            if (i == s) { selX = x; selY = ty; selS = size; }
            x += size + GAP;
        }

        titleAnim = approach(titleAnim, 1, 0.14f);
        float ta = ca * easeOutCubic(titleAnim);
        float dx = (1 - easeOutCubic(titleAnim)) * 12 + slideX;
        float y0 = ROW_Y + TILE_F + 16;
        gfxTextFit(F_TITLE, LEFT + dx, y0, 330, colFade(T.text, ta), g->name);
        char meta[64];
        metaStr(g, meta, sizeof(meta));
        gfxText(F_SMALL, LEFT + dx, y0 + 28, colFade(T.text2, ta), meta);

        playPress = approach(playPress, 0, 0.2f);
        float bs = 1 - playPress * 0.08f;
        const char *pl = "Играть";
        float bw = gfxTextW(F_LABEL, pl) + 52, bh = 28;
        float bx = LEFT + dx + bw * (1 - bs) / 2, by = y0 + 48 + bh * (1 - bs) / 2;
        gfxShadow(bx, by + 2, bw * bs, bh * bs, 6, colFade(T.shadow, ta * 0.6f));
        gfxRound(bx, by, bw * bs, bh * bs, bh * bs / 2, colFade(T.accent, ta));
        gfxText(F_BODY, bx + 16, by + 6, colFade(T.onAccent, ta), G_CROSS);
        gfxText(F_LABEL, bx + 32, by + 6, colFade(T.onAccent, ta), pl);
        float ox = LEFT + dx + bw + 24, oy = y0 + 62;
        gfxCircle(ox, oy, 14, colFade(T.surfaceHi, ta));
        for (int k = -1; k <= 1; k++) gfxCircle(ox + k * 5, oy, 1.6f, colFade(T.text, ta));

        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%d / %d", s + 1, n);
        gfxTextR(F_SMALL, SCR_W - LEFT, y0 + 6, colFade(T.text3, ca), cnt);
    }

    if (cfg.hints) {
        float hx = LEFT;
        hx = drawHint(hx, 252, G_CROSS, "Играть", a);
        hx = drawHint(hx, 252, G_TRIANGLE, "Информация", a);
        hx = drawHint(hx, 252, NULL, "L/R  Вкладки", a);
        hx = drawHint(hx, 252, NULL, "SELECT  Центр", a);
        drawHint(hx, 252, NULL, "START  Настройки", a);
    }
}

/* ================================================================ overlays */
static void wrapText(int font, float x, float y, float w, Color c, const char *s, int maxLines) {
    char line[128];
    int lh = gfxLineH(font);
    while (*s && maxLines-- > 0) {
        int n = 0;
        while (s[n] && n < 126) {
            memcpy(line, s, n + 1);
            line[n + 1] = 0;
            if (gfxTextW(font, line) > w) break;
            n++;
        }
        if (n == 0) n = 1;
        memcpy(line, s, n);
        line[n] = 0;
        gfxText(font, x, y, c, line);
        s += n;
        y += lh;
    }
}

static void drawInfo(void) {
    if (ovInfo < 0.01f) return;
    Game *g = curGame();
    if (!g) return;
    float e = easeOutCubic(ovInfo);
    gfxRect(0, 0, SCR_W, SCR_H, colFade(T.scrim, e));
    float h = 160, y = SCR_H - h * e;
    gfxShadow(14, y, 452, h + 30, 12, colFade(T.shadow, e));
    gfxRound(14, y, 452, h + 30, 18, colFade(T.sheet, e));
    gfxRound(225, y + 7, 30, 4, 2, colFade(T.text3, e));
    float ix = 32, iy = y + 24;
    gfxRound(ix, iy, 100, 100, cfg.radius * 100 / 74.0f, colFade(g->iconState == ICON_READY ? g->iconAvg : g->color, e));
    if (g->iconState == ICON_READY) drawIconFit(g, ix, iy, 100, e);
    float tx = 148;
    gfxTextFit(F_TITLE, tx, iy - 2, 300, colFade(T.text, e), g->name);
    char meta[64];
    metaStr(g, meta, sizeof(meta));
    gfxText(F_BODY, tx, iy + 26, colFade(T.text2, e), meta);
    if (g->discId[0]) {
        char id[32];
        snprintf(id, sizeof(id), "ID: %s%s", g->discId, g->patched ? " • EBOOT.OLD" : "");
        gfxText(F_SMALL, tx, iy + 44, colFade(T.text3, e), id);
    }
    gfxText(F_SMALL, tx, iy + 60, colFade(T.text3, e), "Расположение");
    wrapText(F_SMALL, tx, iy + 74, 300, colFade(T.text2, e), g->path, 2);
    drawHint(tx, y + h - 22, G_CIRCLE, "Закрыть", e);
}

static const char *ccLabel(int i) {
    static const char *l[5] = {"Настройки", "Обновить", NULL, "Сон", "Выйти"};
    if (i == 2) return cfg.sfx || cfg.music ? "Звук: вкл" : "Звук: выкл";
    return l[i];
}

static void drawCC(void) {
    if (ovCC < 0.01f) return;
    float e = easeOutCubic(ovCC);
    gfxRect(0, 0, SCR_W, SCR_H, colFade(T.scrim, e * 0.8f));
    float h = 104, y = SCR_H - h * e;
    gfxShadow(0, y, SCR_W, h + 30, 14, colFade(T.shadow, e));
    gfxRound(0, y, SCR_W, h + 30, 18, colFade(T.sheet, e));
    ScePspDateTime t;
    nowTime(&t);
    static const char *months[12] = {"января", "февраля", "марта", "апреля", "мая", "июня", "июля",
                                     "августа", "сентября", "октября", "ноября", "декабря"};
    static const char *wd[7] = {"вс", "пн", "вт", "ср", "чт", "пт", "сб"};
    char buf[48];
    timeStr(buf, sizeof(buf));
    gfxText(F_TITLE, 24, y + 12, colFade(T.text, e), buf);
    float w = gfxTextW(F_TITLE, buf);
    int dow = sceRtcGetDayOfWeek(t.year, t.month, t.day);
    snprintf(buf, sizeof(buf), "%d %s, %s • " SYS_FULL, t.day, months[(t.month + 11) % 12], wd[(dow % 7 + 7) % 7]);
    gfxText(F_SMALL, 24 + w + 10, y + 21, colFade(T.text2, e), buf);
    int pct = batteryPct();
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%%s", pct, batteryCharging() ? " • заряжается" : "");
    else snprintf(buf, sizeof(buf), "от сети");
    gfxTextR(F_SMALL, SCR_W - 24, y + 21, colFade(T.text2, e), buf);

    float cw = 80, gap = 8, x0 = 24, cy = y + 46, chh = 44;
    ccFocusX = approach(ccFocusX, x0 + ccSel * (cw + gap), 0.25f);
    for (int i = 0; i < 5; i++) gfxRound(x0 + i * (cw + gap), cy, cw, chh, 12, colFade(T.surface, e));
    gfxShadow(ccFocusX, cy + 2, cw, chh, 6, colFade(T.shadow, e * 0.6f));
    gfxRound(ccFocusX, cy, cw, chh, 12, colFade(T.accent, e));
    for (int i = 0; i < 5; i++) {
        float cx = x0 + i * (cw + gap);
        float d = clamp01(1 - fabsf(ccFocusX - cx) / (cw + gap));
        gfxTextC(F_SMALL, cx + cw / 2, cy + 16, colFade(colLerp(T.text, T.onAccent, d), e), ccLabel(i));
    }
}

static void drawToast(void) {
    if (toastT > 0) toastT--;
    toastA = approach(toastA, toastT > 0 ? 1.0f : 0.0f, 0.15f);
    if (toastA < 0.01f) return;
    float e = easeOutCubic(toastA);
    float w = gfxTextW(F_SMALL, toastMsg) + 28;
    float x = SCR_W - (w + 12) * e;
    gfxShadow(x, 40, w, 26, 6, colFade(T.shadow, e));
    gfxRound(x, 40, w, 26, 13, colFade(T.sheet, e));
    gfxText(F_SMALL, x + 14, 46, colFade(T.text, e), toastMsg);
}

static void drawAll(float a) {
    /* when the settings page fully covers the home screen, the tiles, art and
       text underneath are never seen: draw only the background */
    if (ovSet > 0.99f) drawBackdrop(tintCur, 1);
    else {
        drawHome(a);
        drawInfo();
        drawCC();
    }
    settingsDraw(ovSet);
    drawToast();
}

/* ================================================================ launching */
static void zoomFrame(Game *g, float t, float black) {
    beginFrame();
    if (t < 1) drawAll(1 - t);   /* at t = 1 the opaque tile covers the screen */
    float e = easeInOutCubic(t);
    float x = lerpf(selX, 0, e), y = lerpf(selY, 0, e), w = lerpf(selS, SCR_W, e), h = lerpf(selS, SCR_H, e);
    Color base = g->iconState == ICON_READY ? g->iconAvg : g->color;
    gfxRoundGradV(x, y, w, h, lerpf(cfg.radius, 0, e), colLerp(base, WHITE, 0.06f), colLerp(base, BLACK, 0.35f));
    if (g->iconState == ICON_READY) {
        float iw = lerpf(selS, 288, e);
        float sc = iw / g->icon.w;
        if (g->icon.h * sc > lerpf(selS, 160, e)) sc = lerpf(selS, 160, e) / g->icon.h;
        float dw = g->icon.w * sc, dh = g->icon.h * sc;
        gfxImage(&g->icon, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, WHITE);
    }
    if (black > 0) gfxRect(0, 0, SCR_W, SCR_H, colFade(BLACK, black));
    endFrame();
}

static int fileExists(const char *p) {
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

static void launch(Game *g) {
    sfx(S_LAUNCH);
    audioDuckMusic(1);
    playPress = 1;
    int frames = animMul > 10 ? 1 : (int)(30 / animMul);
    for (int f = 0; f <= frames && appRunning; f++) zoomFrame(g, (float)f / frames, 0);
    for (int f = 0; f <= 20; f++) zoomFrame(g, 1, f / 20.0f);

    int go = strncmp(g->path, "ef0", 3) == 0;
    static char pboot[96], argp[192];
    pboot[0] = 0;
    if (g->discId[0] && (g->type == T_ISO || g->type == T_PSN)) {
        snprintf(pboot, sizeof(pboot), "%s:/PSP/GAME/%s/PBOOT.PBP", go ? "ef0" : "ms0", g->discId);
        if (!fileExists(pboot)) pboot[0] = 0;
    }

    struct SceKernelLoadExecVSHParam p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    int apitype;
    switch (g->type) {
    case T_ISO:
        if (pboot[0]) { snprintf(argp, sizeof(argp), "%s", pboot); apitype = go ? RL_ISO_PBOOT_GO : RL_ISO_PBOOT; }
        else { snprintf(argp, sizeof(argp), "%s", g->patched ? UMD_EBOOT_OLD : UMD_EBOOT_BIN); apitype = go ? RL_ISO_GO : RL_ISO; }
        p.key = "umdemu";
        break;
    case T_PSN:
        if (pboot[0]) { snprintf(argp, sizeof(argp), "%s", pboot); apitype = go ? RL_ISO_PBOOT_GO : RL_ISO_PBOOT; }
        else { snprintf(argp, sizeof(argp), "%s", UMD_EBOOT_BIN); apitype = go ? RL_ISO_GO : RL_ISO; }
        p.key = "umdemu";
        break;
    case T_PS1:
        snprintf(argp, sizeof(argp), "%s", g->path);
        apitype = go ? RL_POPS_GO : RL_POPS;
        p.key = "pops";
        break;
    default:
        snprintf(argp, sizeof(argp), "%s", g->path);
        apitype = RL_HOMEBREW;
        p.key = "game";
        break;
    }
    p.argp = argp;
    p.args = strlen(argp) + 1;

    cfgSave();
    audioShutdown();
    if (g->type == T_ISO) {
        sctrlSESetDiscType(DISC_TYPE_GAME);
        sctrlSESetBootConfFileIndex(MODE_INFERNO);
        sctrlSESetUmdFile(g->path);
    } else if (g->type == T_PSN) {
        sctrlSESetBootConfFileIndex(MODE_UMD);
        sctrlSESetUmdFile((char *)"");
    }
    int r = sctrlKernelLoadExecVSHWithApitype(apitype, g->path, &p);

    /* only reached when launching failed */
    audioInit();
    cfgApply();
    audioDuckMusic(0);
    sfx(S_ERROR);
    char m[64];
    snprintf(m, sizeof(m), "Не удалось запустить (0x%08X)", (unsigned)r);
    toast(m);
    for (int f = 30; f >= 0 && appRunning; f--) zoomFrame(g, f / 30.0f, f > 18 ? (f - 18) / 12.0f : 0);
}

/* ================================================================ loading / rescan */
static void waitScan(const char *msg) {
    while (!libScanDone() && appRunning) {
        pollPad();
        beginFrame();
        drawBackdrop(T.bgTop, 1);
        for (int k = 0; k < 8; k++) {
            float ang = frameCount * 0.12f + k * 0.785f;
            gfxCircle(240 + cosf(ang) * 12, 124 + sinf(ang) * 12, 2.2f, colFade(T.text, 0.15f + k * 0.1f));
        }
        gfxTextC(F_SMALL, 240, 150, T.text2, msg);
        endFrame();
    }
}

/* ================================================================ boot */
/* logo -> (library scanned) -> logo zooms out into the home screen. No login:
   this is system software, there are no accounts. */
static void bootScreen(void) {
    int f = 0, outAt = -1;
    sfx(S_BOOT);
    while (appRunning) {
        pollPad();
        if (outAt < 0 && libScanDone() && (f > 100 || (f > 30 && pressed(PSP_CTRL_CROSS)))) {
            outAt = f;
            sfx(S_LOGIN);
        }
        beginFrame();
        drawBackdrop(T.bgTop, clamp01(f / 60.0f));
        float lin = easeOutCubic(f / 50.0f);
        float out = outAt >= 0 ? easeInOutCubic((f - outAt) / 24.0f) : 0;
        float ls = lerpf(0.9f, 1.0f, lin) * (1 + out * 0.35f);
        float la = lin * (1 - out);
        gfxGlow(240, 112, (150 + sinf(f * 0.04f) * 10) * (1 + out), colWithA(T.accent, (int)(30 * la)));
        gfxTextScaled(F_LOGO, 240, 112, ls, colFade(T.text, la), SYS_NAME);
        float va = clamp01((f - 25) / 30.0f) * (1 - out);
        gfxTextC(F_SMALL, 240, 148 + (1 - easeOutCubic(clamp01((f - 25) / 30.0f))) * 6, colFade(T.text3, va), "Системное ПО " SYS_FULL);
        if (!libScanDone() && f > 60) {
            float sa = clamp01((f - 60) / 20.0f);
            for (int k = 0; k < 8; k++) {
                float ang = f * 0.12f + k * 0.785f;
                gfxCircle(240 + cosf(ang) * 8, 200 + sinf(ang) * 8, 1.6f, colFade(T.text, sa * (0.15f + k * 0.1f)));
            }
            gfxTextC(F_SMALL, 240, 216, colFade(T.text3, sa), "Поиск игр…");
        }
        endFrame();
        f++;
        if (outAt >= 0 && f - outAt > 24) break;
    }
}

/* ================================================================ home loop */
static void selectionChanged(void) {
    titleAnim = 0;
    settle = 0;
    libFocus(tab, sel[tab]);
}

static int rescanRequested = 0;
void requestRescan(void) { rescanRequested = 1; }

static void doRescan(void) {
    resetArt();
    libRescan();
    waitScan("Обновление библиотеки…");
    for (int t = 0; t < 2; t++) {
        if (sel[t] >= ngames[t]) sel[t] = ngames[t] ? ngames[t] - 1 : 0;
        cam[t] = sel[t] * (TILE_S + GAP);
    }
    selectionChanged();
    introFrame = frameCount;
    char m[64];
    snprintf(m, sizeof(m), "Игр: %d • Приложений: %d", ngames[0], ngames[1]);
    toast(m);
}

static void homeLoop(void) {
    introFrame = frameCount;
    contentA = 0;
    while (appRunning) {
        pollPad();
        int n = ngames[tab];
        settle++;

        if (overlay == OV_SETTINGS) {
            if (!settingsUpdate()) overlay = OV_NONE;
            if (rescanRequested) { rescanRequested = 0; doRescan(); }
        } else if (overlay == OV_CC) {
            if (repeatBtn(PSP_CTRL_LEFT)) { if (ccSel > 0) { ccSel--; sfx(S_MOVE); } else sfx(S_EDGE); }
            if (repeatBtn(PSP_CTRL_RIGHT)) { if (ccSel < 4) { ccSel++; sfx(S_MOVE); } else sfx(S_EDGE); }
            if (pressed(PSP_CTRL_CIRCLE) || pressed(PSP_CTRL_SELECT)) { overlay = OV_NONE; sfx(S_CLOSE); }
            if (pressed(PSP_CTRL_CROSS)) {
                switch (ccSel) {
                case 0: overlay = OV_SETTINGS; settingsOpen(); sfx(S_OPEN); break;
                case 1:
                    overlay = OV_NONE;
                    sfx(S_OK);
                    doRescan();
                    break;
                case 2: {
                    int on = !(cfg.sfx || cfg.music);
                    cfg.sfx = cfg.music = on;
                    cfgApply();
                    sfx(S_TOGGLE);
                    cfgSave();
                    break;
                }
                case 3: overlay = OV_NONE; cfgSave(); scePowerRequestSuspend(); break;
                case 4:
                    cfgSave();
                    sfx(S_BACK);
                    for (int f = 0; f <= 20; f++) {
                        beginFrame();
                        drawAll(1);
                        gfxRect(0, 0, SCR_W, SCR_H, colFade(BLACK, f / 20.0f));
                        endFrame();
                    }
                    sceKernelExitGame();
                    break;
                }
            }
        } else if (overlay == OV_INFO) {
            if (pressed(PSP_CTRL_CIRCLE) || pressed(PSP_CTRL_TRIANGLE)) { overlay = OV_NONE; sfx(S_CLOSE); }
            if (pressed(PSP_CTRL_CROSS) && n) { overlay = OV_NONE; launch(curGame()); }
        } else {
            if (repeatBtn(PSP_CTRL_LEFT)) {
                if (sel[tab] > 0) { sel[tab]--; sfx(S_MOVE); selectionChanged(); } else sfx(S_EDGE);
            }
            if (repeatBtn(PSP_CTRL_RIGHT)) {
                if (sel[tab] < n - 1) { sel[tab]++; sfx(S_MOVE); selectionChanged(); } else sfx(S_EDGE);
            }
            int lt = pressed(PSP_CTRL_LTRIGGER), rt = pressed(PSP_CTRL_RTRIGGER);
            if (lt || rt) {
                int nt = rt ? 1 : 0;
                if (nt != tab) {
                    tab = nt;
                    slideX = (rt ? 1 : -1) * 40.0f;
                    contentA = 0;
                    introFrame = frameCount;
                    sfx(S_TAB);
                    selectionChanged();
                } else sfx(S_EDGE);
            }
            if (pressed(PSP_CTRL_TRIANGLE) && n) { overlay = OV_INFO; sfx(S_OPEN); }
            if (pressed(PSP_CTRL_SELECT)) { overlay = OV_CC; ccSel = 0; sfx(S_OPEN); }
            if (pressed(PSP_CTRL_START)) { overlay = OV_SETTINGS; settingsOpen(); sfx(S_OPEN); }
            if (pressed(PSP_CTRL_CROSS) && n) launch(curGame());
        }

        if (lastSel != sel[tab] || lastTab != tab) { lastSel = sel[tab]; lastTab = tab; libFocus(tab, sel[tab]); }
        ovInfo = approach(ovInfo, overlay == OV_INFO ? 1.0f : 0.0f, 0.2f);
        ovCC = approach(ovCC, overlay == OV_CC ? 1.0f : 0.0f, 0.2f);
        ovSet = approach(ovSet, overlay == OV_SETTINGS ? 1.0f : 0.0f, 0.2f);
        artUpdate();

        beginFrame();
        drawAll(1);
        endFrame();
    }
}

/* ================================================================ main */
int main(int argc, char *argv[]) {
    int th = sceKernelCreateThread("CbThread", cbThread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);

    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    srand(sceKernelGetSystemTimeLow());

    char self[256] = "ms0:/PSP/GAME/PS5SHELL/EBOOT.PBP";
    if (argc > 0 && argv[0] && argv[0][0]) snprintf(self, sizeof(self), "%s", argv[0]);
    snprintf(appDir, sizeof(appDir), "%s", self);
    char *sl = strrchr(appDir, '/');
    if (sl) *sl = 0;

    cfgDefaults();
    cfgLoad();
    if (cfg.theme < THEME_CUSTOM) T = presets[cfg.theme];
    else T = presets[0];
    gfxInit();
    particlesInit();
    audioInit();
    libStart(self, appDir);
    cfgApply();
    tab = cfg.startTab;

    bootScreen();
    waitScan("Поиск игр…");
    libFocus(tab, 0);
    homeLoop();

    audioShutdown();
    sceKernelExitGame();
    return 0;
}
