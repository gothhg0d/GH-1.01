/* GH-1.01 settings: category sidebar + live content panel.
   Тема (theme cards + color swatches), Обои (thumbnail grid), Интерфейс,
   Звук, Профиль, Система. Every move has a sound and an animation. */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "audio.h"
#include "library.h"
#include "version.h"

/* ================================================================ layout */
#define SB_X 12.0f      /* sidebar */
#define SB_W 140.0f
#define SB_Y 48.0f
#define SB_H 32.0f
#define PX 162.0f       /* content panel */
#define PY 44.0f
#define PW 306.0f
#define PH 200.0f
#define IX (PX + 14)
#define IW (PW - 28)

enum { CAT_THEME, CAT_WALL, CAT_UI, CAT_SOUND, CAT_SYSTEM, NCAT };
static const char *const catNames[NCAT] = {"Тема", "Обои", "Интерфейс", "Звук", "Система"};

enum { R_CHOICE, R_SLIDER, R_TOGGLE, R_ACTION };
enum {
    ID_WALLDIM, ID_WALLBLUR, ID_GAMEART, ID_TINT, ID_PARTICLES, ID_TILESIZE, ID_RADIUS, ID_ANIM,
    ID_HINTS, ID_CLOCK, ID_STARTTAB, ID_SFX, ID_SFXVOL, ID_MUSIC, ID_MUSICVOL, ID_RESCAN, ID_RESET, NIDS
};
typedef struct { int type, id; const char *label; } Row;

static const Row wallRows[] = {
    {R_SLIDER, ID_WALLDIM, "Затемнение"}, {R_CHOICE, ID_WALLBLUR, "Размытие"},
    {R_TOGGLE, ID_GAMEART, "Фон из игры (PIC1)"}, {R_SLIDER, ID_TINT, "Подкраска цветом игры"},
};
static const Row uiRows[] = {
    {R_CHOICE, ID_PARTICLES, "Частицы"}, {R_CHOICE, ID_TILESIZE, "Размер плиток"},
    {R_SLIDER, ID_RADIUS, "Скругление углов"}, {R_CHOICE, ID_ANIM, "Анимации"},
    {R_TOGGLE, ID_HINTS, "Подсказки кнопок"}, {R_CHOICE, ID_CLOCK, "Формат времени"},
    {R_CHOICE, ID_STARTTAB, "Вкладка при запуске"},
};
static const Row soundRows[] = {
    {R_TOGGLE, ID_SFX, "Звуковые эффекты"}, {R_SLIDER, ID_SFXVOL, "Громкость эффектов"},
    {R_TOGGLE, ID_MUSIC, "Фоновая музыка"}, {R_SLIDER, ID_MUSICVOL, "Громкость музыки"},
};
static const Row sysRows[] = {
    {R_ACTION, ID_RESCAN, "Обновить библиотеку"}, {R_ACTION, ID_RESET, "Сбросить все настройки"},
};
#define NROWS(a) (int)(sizeof(a) / sizeof(a[0]))

static const char *const optBlur[] = {"Нет", "Слабое", "Среднее", "Сильное"};
static const char *const optParticles[] = {"Нет", "Мало", "Средне", "Много"};
static const char *const optTiles[] = {"Маленькие", "Средние", "Большие"};
static const char *const optAnim[] = {"Медленно", "Обычно", "Быстро", "Выключены"};
static const char *const optClock[] = {"24 часа", "12 часов"};
static const char *const optTab[] = {"Игры", "Приложения"};

static const Color palette[12] = {
    RGB(255, 255, 255), RGB(160, 162, 170), RGB(44, 46, 54), RGB(0, 0, 0),
    RGB(230, 22, 48), RGB(255, 128, 0), RGB(255, 204, 0), RGB(40, 190, 90),
    RGB(0, 180, 190), RGB(0, 112, 220), RGB(110, 70, 220), RGB(230, 70, 160),
};
static const char *const swatchNames[5] = {"Фон 1", "Фон 2", "Акцент", "Текст", "Рамка"};

/* ================================================================ value access */
static int choiceCount(int id) {
    switch (id) {
    case ID_WALLBLUR: case ID_PARTICLES: case ID_ANIM: return 4;
    case ID_TILESIZE: return 3;
    default: return 2;
    }
}
static int choiceGet(int id) {
    switch (id) {
    case ID_WALLBLUR: return cfg.wallBlur;
    case ID_PARTICLES: return cfg.particles;
    case ID_TILESIZE: return cfg.tileSize;
    case ID_ANIM: return cfg.animSpeed;
    case ID_CLOCK: return cfg.clock24 ? 0 : 1;
    case ID_STARTTAB: return cfg.startTab;
    }
    return 0;
}
static void choiceSet(int id, int v) {
    switch (id) {
    case ID_WALLBLUR: cfg.wallBlur = v; break;
    case ID_PARTICLES: cfg.particles = v; break;
    case ID_TILESIZE: cfg.tileSize = v; break;
    case ID_ANIM: cfg.animSpeed = v; break;
    case ID_CLOCK: cfg.clock24 = v == 0; break;
    case ID_STARTTAB: cfg.startTab = v; break;
    }
}
static const char *choiceText(int id, int v) {
    switch (id) {
    case ID_WALLBLUR: return optBlur[v];
    case ID_PARTICLES: return optParticles[v];
    case ID_TILESIZE: return optTiles[v];
    case ID_ANIM: return optAnim[v];
    case ID_CLOCK: return optClock[v];
    default: return optTab[v];
    }
}
static int *sliderRef(int id, int *lo, int *hi, int *step) {
    *lo = 0; *hi = 100; *step = 10;
    switch (id) {
    case ID_WALLDIM: *hi = 90; return &cfg.wallDim;
    case ID_TINT: return &cfg.tint;
    case ID_RADIUS: *hi = 20; *step = 2; return &cfg.radius;
    case ID_SFXVOL: return &cfg.sfxVol;
    case ID_MUSICVOL: return &cfg.musicVol;
    }
    return NULL;
}
static int *toggleRef(int id) {
    switch (id) {
    case ID_GAMEART: return &cfg.gameArt;
    case ID_HINTS: return &cfg.hints;
    case ID_SFX: return &cfg.sfx;
    case ID_MUSIC: return &cfg.music;
    }
    return NULL;
}

/* theme color slot 0..4 */
static Color colorGet(int slot) {
    Color b[5];
    themeBase(cfg.theme, b);
    return b[slot];
}
static void colorSet(int slot, Color c) {
    c |= 0xFF000000;
    if (cfg.theme != THEME_CUSTOM) { /* editing a preset turns it into "Своя" */
        Color b[5];
        themeBase(cfg.theme, b);
        cfg.cBgTop = b[0]; cfg.cBgBot = b[1]; cfg.cAccent = b[2]; cfg.cText = b[3]; cfg.cFocus = b[4];
        cfg.theme = THEME_CUSTOM;
    }
    Color *dst[5] = {&cfg.cBgTop, &cfg.cBgBot, &cfg.cAccent, &cfg.cText, &cfg.cFocus};
    *dst[slot] = c;
}

static void rgb2hsv(Color c, float *h, float *s, float *v) {
    float r = (c & 255) / 255.0f, g = ((c >> 8) & 255) / 255.0f, b = ((c >> 16) & 255) / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) return; /* greys keep the previous hue */
    float hh = mx == r ? (g - b) / d : (mx == g ? 2 + (b - r) / d : 4 + (r - g) / d);
    hh *= 60;
    if (hh < 0) hh += 360;
    *h = hh;
}
static Color hsv2rgb(float h, float s, float v) {
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return RGB((int)((r + m) * 255 + 0.5f), (int)((g + m) * 255 + 0.5f), (int)((b + m) * 255 + 0.5f));
}

static void hexStr(Color c, char *b) {
    sprintf(b, "#%02X%02X%02X", (unsigned)(c & 255), (unsigned)((c >> 8) & 255), (unsigned)((c >> 16) & 255));
}
static const char *baseName(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* ================================================================ state */
typedef struct { float x, y, w, h, r; } Rect;

static int cat = 0, zone = 0;           /* zone 0 = sidebar, 1 = content */
static float sideY = SB_Y, catA = 1, zoneA = 0;
static Rect fr, frT;                    /* animated focus ring (screen coords) */
static int frSnap = 1;
static float scrollY = 0, scrollT = 0;
static int resetArmed = -1000;

static int thRow = 0, thCard = 0, thSw = 0;
static float cardS[5], cardBadge[5], swS[5];
static Color swDisp[5];

static int wpSel = 0, wpZone = 0, wpRow = 0;
static float thumbS[MAXWALL + 1], thumbA[MAXWALL + 1], wallBadge[MAXWALL + 1];

static int rowSel[NCAT];
static float knob[NIDS], tog[NIDS], valSlide[NIDS];


/* color editor (bottom sheet) */
static int ceOpen = 0, ceSlot = 0, ceRow = 0, cePal = 0, ceTheme = 0;
static float ceA = 0, ceH, ceS, ceV, ceKnob[3];
static Color ceOrig, ceOrigCustom[5];
static Rect ceFr;

static void setFocus(float x, float y, float w, float h, float r) {
    frT.x = x; frT.y = y; frT.w = w; frT.h = h; frT.r = r;
    if (frSnap) { fr = frT; frSnap = 0; }
}

static int curWallIndex(void) {
    for (int i = 0; i < nwall; i++) if (!strcmp(wallFiles[i], cfg.wallpaper)) return i + 1;
    return 0;
}

void settingsOpen(void) {
    zone = 0;
    ceOpen = 0;
    ceA = 0;
    frSnap = 1;
    thCard = cfg.theme;
    wpSel = curWallIndex();
    for (int i = 0; i < 5; i++) swDisp[i] = colorGet(i);
    for (int i = 0; i < NIDS; i++) {
        int lo, hi, st, *v = sliderRef(i, &lo, &hi, &st), *t = toggleRef(i);
        if (v) knob[i] = (float)(*v - lo) / (hi - lo);
        if (t) tog[i] = (float)*t;
    }
}

/* ================================================================ input helpers */
static void changed(void) { cfgApply(); cfgSave(); }

static int rowInput(const Row *r) {
    int d = repeatBtn(PSP_CTRL_RIGHT) - repeatBtn(PSP_CTRL_LEFT);
    int x = pressed(PSP_CTRL_CROSS);
    switch (r->type) {
    case R_CHOICE:
        if (d || x) {
            int n = choiceCount(r->id), v = choiceGet(r->id), dir = d ? d : 1;
            choiceSet(r->id, (v + dir + n) % n);
            valSlide[r->id] = (float)dir;
            sfxPitch(S_TOGGLE, 0.9f + 0.1f * choiceGet(r->id));
            changed();
        }
        break;
    case R_SLIDER: {
        int lo, hi, st, *v = sliderRef(r->id, &lo, &hi, &st);
        if (v && d) {
            int nv = *v + d * st;
            if (nv < lo || nv > hi) sfx(S_EDGE);
            else { *v = nv; sfxPitch(S_TOGGLE, 0.6f + (float)(nv - lo) / (hi - lo)); changed(); }
        }
        break;
    }
    case R_TOGGLE:
        if (d || x) {
            int *v = toggleRef(r->id);
            *v = !*v;
            sfxPitch(S_TOGGLE, *v ? 1.25f : 0.8f);
            changed();
        }
        break;
    case R_ACTION:
        if (!x) break;
        if (r->id == ID_RESCAN) { sfx(S_OK); requestRescan(); }
        else if (r->id == ID_RESET) {
            if (frameCount - resetArmed < 150) {
                cfgDefaults();
                changed();
                settingsOpen();
                zone = 1;
                sfx(S_OK);
                toast("Настройки сброшены");
                resetArmed = -1000;
            } else {
                resetArmed = frameCount;
                sfx(S_ERROR);
                toast("Нажмите " G_CROSS " ещё раз для сброса");
            }
        }
        break;
    }
    return 0;
}

static int listNav(int *sel, int n) {
    if (repeatBtn(PSP_CTRL_UP)) { if (*sel > 0) { (*sel)--; sfx(S_MOVE); } else sfx(S_EDGE); return -1; }
    if (repeatBtn(PSP_CTRL_DOWN)) { if (*sel < n - 1) { (*sel)++; sfx(S_MOVE); } else sfx(S_EDGE); return 1; }
    return 0;
}

static void openColor(int slot) {
    ceSlot = slot;
    ceRow = 0;
    ceOrig = colorGet(slot);
    ceTheme = cfg.theme;
    themeBase(THEME_CUSTOM, ceOrigCustom);
    rgb2hsv(ceOrig, &ceH, &ceS, &ceV);
    ceKnob[0] = ceH / 360; ceKnob[1] = ceS; ceKnob[2] = ceV;
    cePal = 0;
    for (int i = 0; i < 12; i++) if ((palette[i] & 0xFFFFFF) == (ceOrig & 0xFFFFFF)) cePal = i;
    ceFr.w = 0; /* snap the focus ring on open */
    ceOpen = 1;
    sfx(S_OPEN);
}

static void colorInput(void) {
    if (repeatBtn(PSP_CTRL_UP)) { if (ceRow > 0) { ceRow--; sfx(S_MOVE); } else sfx(S_EDGE); }
    if (repeatBtn(PSP_CTRL_DOWN)) { if (ceRow < 3) { ceRow++; sfx(S_MOVE); } else sfx(S_EDGE); }
    int d = repeatBtn(PSP_CTRL_RIGHT) - repeatBtn(PSP_CTRL_LEFT);
    int fast = heldFrames(PSP_CTRL_LEFT) > 40 || heldFrames(PSP_CTRL_RIGHT) > 40;
    if (d) {
        if (ceRow == 0) ceH = fmodf(ceH + d * (fast ? 12 : 4) + 360, 360);
        else if (ceRow == 1) ceS = clamp01(ceS + d * (fast ? 0.06f : 0.02f));
        else if (ceRow == 2) ceV = clamp01(ceV + d * (fast ? 0.06f : 0.02f));
        else { cePal = (cePal + d + 12) % 12; rgb2hsv(palette[cePal], &ceH, &ceS, &ceV); }
        colorSet(ceSlot, hsv2rgb(ceH, ceS, ceV));
        sfxPitch(S_TOGGLE, 0.6f + (ceRow == 0 ? ceH / 360.0f : (ceRow == 1 ? ceS : (ceRow == 2 ? ceV : cePal / 11.0f))) * 0.8f);
    }
    if (pressed(PSP_CTRL_CROSS)) { ceOpen = 0; sfx(S_OK); changed(); }
    if (pressed(PSP_CTRL_CIRCLE)) {
        cfg.theme = ceTheme;
        cfg.cBgTop = ceOrigCustom[0]; cfg.cBgBot = ceOrigCustom[1]; cfg.cAccent = ceOrigCustom[2];
        cfg.cText = ceOrigCustom[3]; cfg.cFocus = ceOrigCustom[4];
        ceOpen = 0;
        sfx(S_CLOSE);
    }
}

/* ---------------------------------------------------------------- per category */
static void themeInput(void) {
    listNav(&thRow, 2);
    int d = repeatBtn(PSP_CTRL_RIGHT) - repeatBtn(PSP_CTRL_LEFT);
    if (thRow == 0) {
        if (d) {
            int n = thCard + d;
            if (n < 0 || n > NPRESETS) sfx(S_EDGE);
            else { thCard = n; sfx(S_MOVE); }
        }
        if (pressed(PSP_CTRL_CROSS)) {
            if (cfg.theme != thCard) { cfg.theme = thCard; cardBadge[thCard] = 0; sfx(S_OK); changed(); }
            else sfx(S_TOGGLE);
            cardS[thCard] = 1.18f; /* pop */
        }
    } else {
        if (d) {
            int n = thSw + d;
            if (n < 0 || n > 4) sfx(S_EDGE);
            else { thSw = n; sfx(S_MOVE); }
        }
        if (pressed(PSP_CTRL_CROSS)) openColor(thSw);
    }
}

static int wallRowsN(void) { return (nwall + 1 + 2) / 3; }

static void wallInput(void) {
    int n = nwall + 1;
    libThumbFocus(wpSel > 0 ? wpSel - 1 : 0);
    if (wpZone == 0) {
        int col = wpSel % 3;
        if (repeatBtn(PSP_CTRL_LEFT)) { if (col > 0) { wpSel--; sfx(S_MOVE); } else sfx(S_EDGE); }
        if (repeatBtn(PSP_CTRL_RIGHT)) { if (col < 2 && wpSel + 1 < n) { wpSel++; sfx(S_MOVE); } else sfx(S_EDGE); }
        if (repeatBtn(PSP_CTRL_UP)) { if (wpSel >= 3) { wpSel -= 3; sfx(S_MOVE); } else sfx(S_EDGE); }
        if (repeatBtn(PSP_CTRL_DOWN)) {
            if (wpSel + 3 < n) { wpSel += 3; sfx(S_MOVE); }
            else if (wpSel / 3 < wallRowsN() - 1) { wpSel = n - 1; sfx(S_MOVE); }
            else { wpZone = 1; wpRow = 0; sfx(S_MOVE); }
        }
        if (pressed(PSP_CTRL_CROSS)) {
            const char *p = wpSel ? wallFiles[wpSel - 1] : "";
            if (strcmp(p, cfg.wallpaper)) {
                snprintf(cfg.wallpaper, sizeof(cfg.wallpaper), "%s", p);
                wallBadge[wpSel] = 0;
                changed();
                sfx(S_OK);
            } else sfx(S_TOGGLE);
            thumbS[wpSel] = 1.16f;
        }
    } else {
        if (repeatBtn(PSP_CTRL_UP)) {
            if (wpRow > 0) { wpRow--; sfx(S_MOVE); }
            else { wpZone = 0; sfx(S_MOVE); }
        } else if (repeatBtn(PSP_CTRL_DOWN)) {
            if (wpRow < NROWS(wallRows) - 1) { wpRow++; sfx(S_MOVE); } else sfx(S_EDGE);
        } else rowInput(&wallRows[wpRow]);
    }
}


int settingsUpdate(void) {
    if (ceOpen) { colorInput(); return 1; }
    if (zone == 0) {
        int old = cat;
        if (repeatBtn(PSP_CTRL_UP)) { if (cat > 0) cat--; else sfx(S_EDGE); }
        if (repeatBtn(PSP_CTRL_DOWN)) { if (cat < NCAT - 1) cat++; else sfx(S_EDGE); }
        if (cat != old) { catA = 0; scrollY = scrollT = 0; sfx(S_TAB); }
        if (pressed(PSP_CTRL_CROSS) || pressed(PSP_CTRL_RIGHT)) {
            zone = 1;
            frSnap = 1;
            sfxPitch(S_OPEN, 1.2f);
            if (cat == CAT_THEME) thCard = cfg.theme;
            if (cat == CAT_WALL) { wpSel = curWallIndex(); wpZone = 0; }
        }
        if (pressed(PSP_CTRL_CIRCLE) || pressed(PSP_CTRL_START)) { cfgSave(); sfx(S_CLOSE); return 0; }
        return 1;
    }
    if (pressed(PSP_CTRL_CIRCLE)) { zone = 0; sfx(S_BACK); return 1; }
    if (pressed(PSP_CTRL_START)) { cfgSave(); sfx(S_CLOSE); return 0; }
    switch (cat) {
    case CAT_THEME: themeInput(); break;
    case CAT_WALL: wallInput(); break;
    case CAT_UI: listNav(&rowSel[cat], NROWS(uiRows)); rowInput(&uiRows[rowSel[cat]]); break;
    case CAT_SOUND: listNav(&rowSel[cat], NROWS(soundRows)); rowInput(&soundRows[rowSel[cat]]); break;
    case CAT_SYSTEM: listNav(&rowSel[cat], NROWS(sysRows)); rowInput(&sysRows[rowSel[cat]]); break;
    }
    return 1;
}

/* ================================================================ drawing */
static void drawToggle(float x, float y, float on, float a) {
    gfxRound(x, y, 34, 18, 9, colFade(colLerp(T.surfaceHi, T.accent, on), a));
    float kx = x + 9 + on * 16;
    gfxShadow(kx - 6, y + 3, 12, 12, 2, colFade(T.shadow, a * 0.5f));
    gfxCircle(kx, y + 9, 7, colFade(colLuma(T.accent) > 150 && on > 0.5f ? T.onAccent : WHITE, a));
}

static void catIcon(int c, float cx, float cy, Color col) {
    switch (c) {
    case CAT_THEME:
        gfxCircle(cx, cy, 7, col);
        gfxCircle(cx + 2.5f, cy - 2.5f, 3, colWithA(T.sheet, 255));
        gfxCircle(cx - 3, cy + 2, 1.6f, colWithA(T.sheet, 255));
        break;
    case CAT_WALL:
        gfxRing(cx - 8, cy - 6, 16, 12, 3, col);
        gfxCircle(cx - 3, cy - 1.5f, 1.8f, col);
        gfxRound(cx - 1, cy + 1, 7, 3, 1, col);
        break;
    case CAT_UI:
        for (int i = 0; i < 4; i++) gfxRound(cx - 7 + (i & 1) * 8, cy - 7 + (i >> 1) * 8, 6, 6, 1.5f, col);
        break;
    case CAT_SOUND:
        for (int i = 0; i < 4; i++) {
            float h = 4 + ((i * 5 + 3) % 9) + 2 * sinf(frameCount * 0.1f + i);
            gfxRound(cx - 7 + i * 4, cy + 7 - h, 3, h, 1.5f, col);
        }
        break;
    default:
        gfxRing(cx - 7.5f, cy - 7.5f, 15, 15, 7.5f, col);
        gfxRect(cx - 0.75f, cy - 1, 1.5f, 5, col);
        gfxCircle(cx, cy - 3.5f, 1, col);
        break;
    }
}

/* generic row: label + control. returns nothing; sets focus when focused */
static void drawRow(const Row *r, float x, float y, float w, int focused, float a) {
    if (focused) setFocus(x - 6, y, w + 12, 28, 10);
    gfxText(F_BODY, x, y + 7, colFade(focused ? T.text : T.text2, a), r->label);
    float rx = x + w;
    int id = r->id;
    switch (r->type) {
    case R_TOGGLE:
        tog[id] = approach(tog[id], *toggleRef(id) ? 1.0f : 0.0f, 0.25f);
        drawToggle(rx - 34, y + 5, easeOutCubic(tog[id]), a);
        break;
    case R_SLIDER: {
        int lo, hi, st, *v = sliderRef(id, &lo, &hi, &st);
        float t = (float)(*v - lo) / (hi - lo);
        knob[id] = approach(knob[id], t, 0.3f);
        char b[16];
        snprintf(b, sizeof(b), id == ID_RADIUS ? "%d px" : "%d%%", *v);
        gfxTextR(F_SMALL, rx, y + 9, colFade(T.text2, a), b);
        float bw = 74, bx = rx - 42 - bw;
        gfxRound(bx, y + 12, bw, 4, 2, colFade(T.surfaceHi, a));
        gfxRound(bx, y + 12, bw * knob[id], 4, 2, colFade(T.accent, a));
        float kr = focused ? 6.5f : 5;
        gfxShadow(bx + bw * knob[id] - kr, y + 14 - kr, kr * 2, kr * 2, 2, colFade(T.shadow, a * 0.5f));
        gfxCircle(bx + bw * knob[id], y + 14, kr, colFade(WHITE, a));
        break;
    }
    case R_CHOICE: {
        valSlide[id] = approach(valSlide[id], 0, 0.25f);
        const char *v = choiceText(id, choiceGet(id));
        float w2 = gfxTextW(F_SMALL, v), s = valSlide[id];
        float tx = rx - 16 - w2 + s * 10;
        gfxText(F_SMALL, tx, y + 9, colFade(T.text, a * (1 - fabsf(s) * 0.8f)), v);
        if (focused) {
            gfxTextR(F_SMALL, rx, y + 9, colFade(T.text3, a), "›");
            gfxText(F_SMALL, rx - 16 - w2 - 14, y + 9, colFade(T.text3, a), "‹");
        }
        break;
    }
    case R_ACTION:
        gfxTextR(F_SMALL, rx, y + 9, colFade(T.text3, a), "›");
        break;
    }
}

static void drawRows(const Row *rows, int n, int sel, float x, float y, float w, float a) {
    for (int i = 0; i < n; i++) drawRow(&rows[i], x, y + i * 30, w, zone == 1 && i == sel, a);
}

static void miniTheme(int t, float x, float y, float w, float h, float a) {
    Color b[5];
    themeBase(t, b);
    Theme pt;
    if (t < NPRESETS) pt = presets[t];
    else { pt.surfaceHi = colWithA(b[3], 40); pt.text3 = colWithA(b[3], 105); }
    gfxRoundGradV(x, y, w, h, 8, colFade(b[0], a), colFade(b[1], a));
    float s = w / 48;
    gfxRound(x + 5 * s, y + 6 * s, 12 * s, 12 * s, 3 * s, colFade(pt.surfaceHi, a));
    gfxRing(x + 4 * s, y + 5 * s, 14 * s, 14 * s, 4 * s, colFade(b[4], a));
    gfxRound(x + 21 * s, y + 7 * s, 20 * s, 3 * s, 1.5f * s, colFade(b[3], a));
    gfxRound(x + 21 * s, y + 13 * s, 13 * s, 2.5f * s, 1.2f * s, colFade(pt.text3, a));
    gfxRound(x + 5 * s, y + 24 * s, 18 * s, 6 * s, 3 * s, colFade(b[2], a));
    gfxRing(x - 0.5f, y - 0.5f, w + 1, h + 1, 8, colFade(T.surfaceHi, a));
}

static void badge(float cx, float cy, float k, float a) {
    if (k < 0.02f) return;
    float r = 6.5f * k;
    gfxShadow(cx - r, cy - r + 1, r * 2, r * 2, 2, colFade(T.shadow, a * 0.6f));
    gfxCircle(cx, cy, r, colFade(T.accent, a));
    gfxCircle(cx, cy, 2.2f * k, colFade(T.onAccent, a));
}

static void drawThemeCat(float y0, float a) {
    gfxText(F_SMALL, IX, y0 + 8, colFade(T.text3, a), "Тема");
    float cw = 48, ch = 40, gap = (IW - cw * 5) / 4;
    for (int i = 0; i <= NPRESETS; i++) {
        int foc = zone == 1 && thRow == 0 && thCard == i;
        cardS[i] = approach(cardS[i], foc ? 1.1f : 1.0f, 0.2f);
        cardBadge[i] = approach(cardBadge[i], cfg.theme == i ? 1.0f : 0.0f, 0.2f);
        float s = cardS[i], w = cw * s, h = ch * s;
        float cx = IX + i * (cw + gap) + cw / 2, cy = y0 + 26 + ch / 2;
        gfxShadow(cx - w / 2, cy - h / 2 + 2 + (s - 1) * 20, w, h, 4 + (s - 1) * 40, colFade(T.shadow, a * (0.5f + (s - 1) * 4)));
        miniTheme(i, cx - w / 2, cy - h / 2, w, h, a);
        if (foc) setFocus(cx - w / 2 - 4, cy - h / 2 - 4, w + 8, h + 8, 12);
        float bk = easeOutCubic(cardBadge[i]) * (1 + 0.25f * sinf(cardBadge[i] * 3.14159f));
        badge(cx + w / 2 - 3, cy - h / 2 + 3, bk, a);
    }
    const char *nm = themeNames[zone == 1 && thRow == 0 ? thCard : cfg.theme];
    gfxText(F_LABEL, IX, y0 + 76, colFade(T.text, a), nm);
    if ((zone == 1 && thRow == 0 ? thCard : cfg.theme) == cfg.theme)
        gfxText(F_SMALL, IX + gfxTextW(F_LABEL, nm) + 8, y0 + 78, colFade(T.text3, a), "• используется");

    gfxText(F_SMALL, IX, y0 + 102, colFade(T.text3, a), "Цвета темы");
    for (int i = 0; i < 5; i++) {
        int foc = zone == 1 && thRow == 1 && thSw == i;
        swS[i] = approach(swS[i], foc ? 1.15f : 1.0f, 0.25f);
        swDisp[i] = colLerp(swDisp[i], colorGet(i), clamp01(0.2f * animMul));
        float cx = IX + 26 + i * (IW - 52) / 4, cy = y0 + 136, r = 15 * swS[i];
        gfxShadow(cx - r, cy - r + 2, r * 2, r * 2, 4, colFade(T.shadow, a * 0.6f));
        gfxCircle(cx, cy, r, colFade(swDisp[i], a));
        gfxRing(cx - r - 0.5f, cy - r - 0.5f, r * 2 + 1, r * 2 + 1, r + 0.5f, colFade(T.surfaceHi, a));
        gfxTextC(F_SMALL, cx, y0 + 156, colFade(foc ? T.text : T.text3, a), swatchNames[i]);
        if (foc) setFocus(cx - r - 5, cy - r - 5, r * 2 + 10, r * 2 + 10, r + 5);
    }
    gfxText(F_SMALL, IX, y0 + 176, colFade(T.text3, a * 0.8f), "Изменение цвета создаёт тему «Своя»");
}

static float wallGridTop(float y0) { return y0 + 26; }

static void drawWallCat(float y0, float a) {
    const char *cur = cfg.wallpaper[0] ? baseName(cfg.wallpaper) : "Без обоев";
    gfxText(F_SMALL, IX, y0 + 8, colFade(T.text3, a), "Обои");
    gfxTextFit(F_SMALL, IX + 40, y0 + 8, IW - 40 - 50, colFade(T.text2, a), cur);
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "%d шт.", nwall);
    gfxTextR(F_SMALL, IX + IW, y0 + 8, colFade(T.text3, a), cnt);

    float tw = (IW - 20) / 3, th = tw * 272 / 480, gy = wallGridTop(y0);
    int n = nwall + 1, applied = curWallIndex();
    for (int i = 0; i < n; i++) {
        float x = IX + (i % 3) * (tw + 10), y = gy + (i / 3) * (th + 12);
        if (y > PY + PH + 10 || y + th < PY - 10) continue;
        int foc = zone == 1 && wpZone == 0 && wpSel == i;
        thumbS[i] = approach(thumbS[i], foc ? 1.07f : 1.0f, 0.22f);
        wallBadge[i] = approach(wallBadge[i], applied == i ? 1.0f : 0.0f, 0.2f);
        float s = thumbS[i], w = tw * s, h = th * s, cx = x + tw / 2, cy = y + th / 2;
        float lx = cx - w / 2, ly = cy - h / 2;
        gfxShadow(lx, ly + 2 + (s - 1) * 20, w, h, 4 + (s - 1) * 50, colFade(T.shadow, a * 0.6f));
        if (i == 0) {
            gfxRound(lx, ly, w, h, 8, colFade(T.surfaceHi, a));
            gfxRing(cx - 9, cy - 9, 18, 18, 9, colFade(T.text3, a));
            gfxRound(cx - 6, cy - 0.75f, 12, 1.5f, 0.75f, colFade(T.text3, a));
        } else {
            int st = wallThumbState[i - 1];
            thumbA[i] = approach(thumbA[i], st == ICON_READY ? 1.0f : 0.0f, 0.15f);
            if (thumbA[i] < 0.99f) {
                float sh = 0.5f + 0.5f * sinf(frameCount * 0.12f - i * 0.6f); /* loading shimmer */
                gfxRound(lx, ly, w, h, 8, colFade(colLerp(T.surface, T.surfaceHi, sh), a));
                if (st == ICON_FAILED) gfxTextC(F_SMALL, cx, cy - 6, colFade(T.text3, a), "?");
            }
            if (thumbA[i] > 0.01f) {
                gfxImage(&wallThumb[i - 1], lx, ly, w, h, colFade(WHITE, a * thumbA[i]));
                gfxCornerMask(lx, ly, w, h, 8, colFade(T.sheet, a));
            }
        }
        if (foc) setFocus(lx - 4, ly - 4, w + 8, h + 8, 12);
        badge(lx + w - 7, ly + 7, easeOutCubic(wallBadge[i]), a);
    }
    float ry = gy + wallRowsN() * (th + 12) + 4;
    gfxRect(IX, ry - 4, IW, 1, colFade(T.surfaceHi, a));
    for (int i = 0; i < NROWS(wallRows); i++)
        drawRow(&wallRows[i], IX + 6, ry + i * 30, IW - 12, zone == 1 && wpZone == 1 && wpRow == i, a);
}


static void drawSystemCat(float y0, float a) {
    gfxTextScaled(F_LOGO, IX + 34, y0 + 30, 0.62f, colFade(T.text, a), SYS_NAME);
    gfxText(F_SMALL, IX + 80, y0 + 12, colFade(T.text3, a), "Системное ПО");
    gfxText(F_TITLE, IX + 80, y0 + 24, colFade(T.text, a), SYS_FULL);

    char b[64];
    int fw = sceKernelDevkitVersion();
    snprintf(b, sizeof(b), "Прошивка PSP: %d.%d%d • ARK-4", (fw >> 24) & 15, (fw >> 16) & 15, (fw >> 8) & 15);
    gfxText(F_SMALL, IX, y0 + 60, colFade(T.text2, a), b);

    static u32 freeMB = 0, totalMB = 0;
    static int msChecked = -1000;
    if (frameCount - msChecked > 300) {
        struct { u32 maxClusters, freeClusters; int unk; u32 sectorSize, sectorCount; } dc;
        void *pp = &dc;
        memset(&dc, 0, sizeof(dc));
        if (sceIoDevctl("ms0:", 0x02425818, &pp, sizeof(pp), NULL, 0) >= 0) {
            u64 cl = (u64)dc.sectorSize * dc.sectorCount;
            freeMB = (u32)(((u64)dc.freeClusters * cl) >> 20);
            totalMB = (u32)(((u64)dc.maxClusters * cl) >> 20);
        }
        msChecked = frameCount;
    }
    if (totalMB) {
        snprintf(b, sizeof(b), "Карта памяти: свободно %lu,%lu из %lu,%lu ГБ",
                 (unsigned long)(freeMB / 1024), (unsigned long)(freeMB % 1024 * 10 / 1024),
                 (unsigned long)(totalMB / 1024), (unsigned long)(totalMB % 1024 * 10 / 1024));
        gfxText(F_SMALL, IX, y0 + 76, colFade(T.text2, a), b);
        float used = 1 - (float)freeMB / totalMB;
        gfxRound(IX, y0 + 92, IW, 5, 2.5f, colFade(T.surfaceHi, a));
        gfxRound(IX, y0 + 92, IW * used, 5, 2.5f, colFade(T.accent, a));
    }
    snprintf(b, sizeof(b), "Игры: %d • Приложения: %d • Обои: %d", ngames[0], ngames[1], nwall);
    gfxText(F_SMALL, IX, y0 + 102, colFade(T.text3, a), b);
    drawRows(sysRows, NROWS(sysRows), rowSel[CAT_SYSTEM], IX + 6, y0 + 122, IW - 12, a);
}

static void drawColorSheet(void) {
    ceA = approach(ceA, ceOpen ? 1.0f : 0.0f, 0.22f);
    if (ceA < 0.01f) return;
    float e = easeOutCubic(ceA);
    gfxRect(0, 0, SCR_W, SCR_H, colFade(T.scrim, e * 0.6f));
    float h = 172, y = SCR_H - h * e;
    gfxShadow(0, y, SCR_W, h + 30, 14, colFade(T.shadow, e));
    gfxRound(0, y, SCR_W, h + 30, 18, colFade(T.sheet, e));
    gfxRound(225, y + 6, 30, 4, 2, colFade(T.text3, e));

    Color cur = hsv2rgb(ceH, ceS, ceV);
    const char *title = swatchNames[ceSlot];
    gfxText(F_LABEL, 20, y + 16, colFade(T.text, e), title);
    gfxShadow(20, y + 46, 76, 70, 6, colFade(T.shadow, e * 0.7f));
    gfxRound(20, y + 44, 76, 70, 12, colFade(cur, e));
    gfxRing(19.5f, y + 43.5f, 77, 71, 12, colFade(T.surfaceHi, e));
    char hex[16];
    hexStr(cur, hex);
    gfxText(F_SMALL, 20, y + 122, colFade(T.text2, e), hex);

    static const char *rows[3] = {"Оттенок", "Насыщенность", "Яркость"};
    float bx = 214, bw = 242;
    Rect target = ceFr;
    for (int i = 0; i < 3; i++) {
        float ry = y + 42 + i * 30;
        ceKnob[i] = approach(ceKnob[i], i == 0 ? ceH / 360 : (i == 1 ? ceS : ceV), 0.35f);
        gfxText(F_SMALL, 116, ry + 3, colFade(i == ceRow ? T.text : T.text2, e), rows[i]);
        if (i == 0)
            for (int k = 0; k < 6; k++)
                gfxGradH(bx + bw * k / 6, ry + 5, bw / 6 + 0.5f, 8, colFade(hsv2rgb(k * 60, 1, 1), e), colFade(hsv2rgb(k == 5 ? 359.9f : (k + 1) * 60, 1, 1), e));
        else if (i == 1) gfxGradH(bx, ry + 5, bw, 8, colFade(hsv2rgb(ceH, 0, ceV), e), colFade(hsv2rgb(ceH, 1, ceV), e));
        else gfxGradH(bx, ry + 5, bw, 8, colFade(BLACK, e), colFade(hsv2rgb(ceH, ceS, 1), e));
        gfxCornerMask(bx, ry + 5, bw, 8, 4, colFade(T.sheet, e));
        float kx = bx + bw * ceKnob[i], kr = i == ceRow ? 8 : 6.5f;
        gfxShadow(kx - kr, ry + 9 - kr + 1, kr * 2, kr * 2, 2, colFade(T.shadow, e * 0.6f));
        gfxCircle(kx, ry + 9, kr, colFade(WHITE, e));
        gfxCircle(kx, ry + 9, kr - 2, colFade(cur, e));
        if (i == ceRow) { target.x = 108; target.y = ry - 4; target.w = 358; target.h = 26; target.r = 10; }
    }
    float py = y + 132;
    for (int i = 0; i < 12; i++) {
        float cx = 124 + i * 28.5f, r = (ceRow == 3 && cePal == i) ? 10.5f : 9;
        gfxCircle(cx, py + 10, r, colFade(palette[i], e));
        if (colLuma(palette[i]) < 40 || colLuma(palette[i]) > 230) gfxRing(cx - r, py + 10 - r, r * 2, r * 2, r, colFade(T.surfaceHi, e));
        if (ceRow == 3 && i == cePal) { target.x = cx - 14; target.y = py - 4; target.w = 28; target.h = 28; target.r = 14; }
    }
    if (ceFr.w < 1) ceFr = target;
    ceFr.x = approach(ceFr.x, target.x, 0.3f); ceFr.y = approach(ceFr.y, target.y, 0.3f);
    ceFr.w = approach(ceFr.w, target.w, 0.3f); ceFr.h = approach(ceFr.h, target.h, 0.3f);
    ceFr.r = approach(ceFr.r, target.r, 0.3f);
    if (ceRow < 3) gfxRound(ceFr.x, ceFr.y, ceFr.w, ceFr.h, ceFr.r, colFade(T.surface, e));
    gfxRing(ceFr.x, ceFr.y, ceFr.w, ceFr.h, ceFr.r, colFade(T.focus, e * (0.75f + 0.25f * sinf(frameCount * 0.1f))));
    float hx = SCR_W - 190;
    hx = drawHint(hx, y + 16, G_CROSS, "Готово", e);
    drawHint(hx, y + 16, G_CIRCLE, "Отмена", e);
}

void settingsDraw(float a) {
    if (a < 0.01f) return;
    float e = easeOutCubic(a);
    const float ox = 0; /* the page fades in; content slides vertically per category */
    gfxRect(0, 0, SCR_W, SCR_H, colFade(colWithA(T.bgBot, 200), e));
    gfxText(F_TITLE, 20 + ox, 12, colFade(T.text, e), "Настройки");
    gfxTextR(F_SMALL, SCR_W - 16 + ox, 18, colFade(T.text3, e), SYS_FULL);

    /* sidebar */
    sideY = approach(sideY, SB_Y + cat * SB_H, 0.3f);
    zoneA = approach(zoneA, zone == 0 ? 1.0f : 0.0f, 0.2f);
    gfxShadow(SB_X + ox, sideY + 2, SB_W, SB_H - 4, 5, colFade(T.shadow, e * 0.4f * zoneA));
    gfxRound(SB_X + ox, sideY, SB_W, SB_H - 4, 12, colFade(colLerp(T.surfaceHi, T.accent, zoneA), e));
    for (int i = 0; i < NCAT; i++) {
        float y = SB_Y + i * SB_H;
        float on = clamp01(1 - fabsf(sideY - y) / SB_H);
        Color c = colLerp(i == cat ? T.text : T.text2, T.onAccent, on * zoneA);
        catIcon(i, SB_X + 18 + ox, y + 14, colFade(c, e));
        gfxText(F_LABEL, SB_X + 34 + ox, y + 6, colFade(c, e), catNames[i]);
    }

    /* content panel */
    catA = approach(catA, 1, 0.16f);
    float ca = e * easeOutCubic(catA);
    gfxShadow(PX + ox, PY + 3, PW, PH, 10, colFade(T.shadow, e * 0.8f));
    gfxRound(PX + ox, PY, PW, PH, 16, colFade(T.sheet, e));

    /* scrolling follows the focus ring */
    float contentTop = PY + 6 + (1 - easeOutCubic(catA)) * 14;
    gfxClip((int)(PX + ox), (int)PY + 2, (int)PW, (int)PH - 4);
    float y0 = contentTop - scrollY;
    frT.w = 0;
    switch (cat) {
    case CAT_THEME: drawThemeCat(y0, ca); break;
    case CAT_WALL: drawWallCat(y0, ca); break;
    case CAT_UI: drawRows(uiRows, NROWS(uiRows), rowSel[CAT_UI], IX + 6, y0 + 6, IW - 12, ca); break;
    case CAT_SOUND: drawRows(soundRows, NROWS(soundRows), rowSel[CAT_SOUND], IX + 6, y0 + 6, IW - 12, ca); break;
    case CAT_SYSTEM: drawSystemCat(y0, ca); break;
    }
    if (zone == 1 && frT.w > 0) {
        float fy = frT.y + scrollY;           /* unscrolled position */
        if (fy - contentTop < scrollT + 6) scrollT = fy - contentTop - 6;
        if (fy + frT.h - contentTop > scrollT + PH - 16) scrollT = fy + frT.h - contentTop - PH + 16;
        if (scrollT < 0) scrollT = 0;
        fr.x = approach(fr.x, frT.x, 0.3f); fr.y = approach(fr.y, frT.y, 0.3f);
        fr.w = approach(fr.w, frT.w, 0.3f); fr.h = approach(fr.h, frT.h, 0.3f);
        fr.r = approach(fr.r, frT.r, 0.3f);
        float pulse = 0.75f + 0.25f * sinf(frameCount * 0.1f);
        gfxRing(fr.x + ox, fr.y, fr.w, fr.h, fr.r, colFade(T.focus, ca * pulse * (1 - zoneA)));
    }
    scrollY = approach(scrollY, scrollT, 0.2f);
    gfxNoClip();

    /* hints */
    float hx = 20 + ox;
    if (zone == 0) {
        hx = drawHint(hx, 252, NULL, "↑↓ Раздел", e);
        hx = drawHint(hx, 252, G_CROSS, "Открыть", e);
        drawHint(hx, 252, G_CIRCLE, "Закрыть", e);
    } else {
        hx = drawHint(hx, 252, NULL, "↑↓←→ Выбор", e);
        hx = drawHint(hx, 252, G_CROSS, cat == CAT_THEME && thRow == 1 ? "Изменить цвет" : "Выбрать", e);
        drawHint(hx, 252, G_CIRCLE, "Назад", e);
    }
    drawColorSheet();
}
