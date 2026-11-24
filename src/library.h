/* Game library: scanning, titles, ICON0 icons, PIC1 background art and
   wallpapers. All file IO and image decoding runs in a low-priority loader
   thread so the UI never stalls. */
#pragma once
#include "gfx.h"

/* T_ISO: ISO/CSO/ZSO/DAX/JSO image, T_PSN: PSN/minis EBOOT, T_PS1: POPS EBOOT,
   T_APP: homebrew EBOOT */
enum { T_ISO, T_PSN, T_PS1, T_APP };
enum { ICON_NONE, ICON_READY, ICON_FAILED };

#define MAXG 256
#define ICON_W 144
#define ICON_H 80

typedef struct {
    char name[96];          /* UTF-8, from PARAM.SFO when available */
    char path[192];
    char discId[16];        /* e.g. ULUS10041, used for PBOOT.PBP lookup */
    char fmt[8];            /* "ISO", "CSO", "ZSO", "DAX", "JSO", "PBP" */
    int type;
    int patched;            /* ISO contains SYSDIR/EBOOT.OLD */
    u32 sizeMB;
    Color color;            /* fallback tile color */
    volatile int iconState;
    Texture icon;           /* aspect-correct, fits 144x80, RGBA8888 in RAM */
    Color iconAvg;
    int noArt;              /* PIC1 missing / failed */
    float iconFade;         /* UI animation (main thread only) */
} Game;

extern Game games[2][MAXG]; /* [0] games (ISO, PSN, PS1)  [1] homebrew */
extern int ngames[2];

void libStart(const char *selfPath, const char *appDir);
int  libScanDone(void);
void libRescan(void);               /* main thread must not touch games until libScanDone() */
void libFocus(int tab, int sel);

/* background art: two VRAM slots, crossfade logic lives in main.c */
enum { ART_IDLE, ART_LOADING, ART_READY, ART_FAILED };
extern volatile int artState;
extern volatile int artTarget;
extern Game *volatile artGame;
void libRequestArt(Game *g, int slot);

/* wallpapers: any PNG/JPG, scaled to cover 480x272, optional blur.
   The list is scanned together with the games (valid once libScanDone()). */
#define MAXWALL 128
#define THUMB_W 96
#define THUMB_H 54
extern char wallFiles[MAXWALL][192];
extern int nwall;
extern Texture wallThumb[MAXWALL];          /* RGB565 96x54 */
extern volatile int wallThumbState[MAXWALL]; /* ICON_NONE / ICON_READY / ICON_FAILED */
void libThumbFocus(int idx);                 /* thumbnails near idx load first */
void libRequestWallpaper(const char *path, int blur); /* path "" = none */
int  libWallpaper(Texture *t);    /* 1 if a wallpaper is ready (t filled) */
int  libWallpaperSerial(void);    /* changes every time a new one is ready */
int  libWallpaperBusy(void);
