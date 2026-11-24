#include <pspkernel.h>
#include <pspaudiolib.h>
#include <pspaudio.h>
#include <string.h>
#include <math.h>
#include "audio.h"

#define RATE     44100.0f
#define NSFX     10          /* voices 0..9: sound effects (main thread)    */
#define NV       14          /* voices 10..13: music bells (audio thread)   */
#define TAB_BITS 12
#define TAB_SIZE (1 << TAB_BITS)

enum { W_SINE, W_TRI, W_SQUARE, W_NOISE };

/* Per-sample work is integer phase stepping + a few multiplies: no float->int
   conversions or divisions inside the sample loop (those are slow on the
   PSP's Allegrex FPU). */
typedef struct {
    volatile int on;
    int delay;            /* samples before start */
    int pos, att, len;    /* envelope in samples  */
    u32 phase, inc;       /* phase step = f / RATE * 2^32 */
    int dinc;             /* slide: change of inc per sample */
    float env, dAtt, t, dDec; /* attack ramp, then t: 1 -> 0, env = t*t */
    float amp;
    int wave;
} Voice;

#define HZ2INC(f) ((u32)((f) * (4294967296.0f / RATE)))

static Voice vo[NV];
static float sinTab[TAB_SIZE];
static volatile int sfxOn = 1, musicOn = 1, duck = 0;
static float musicVol = 0.0f;
static volatile float sfxGain = 1.0f, musicGain = 1.0f;
static u32 noiseState = 0x12345678;
static int inited = 0;

/* ---------------------------------------------------------------- music */
static const float chords[4][4] = {
    {130.81f, 196.00f, 246.94f, 329.63f}, /* Cmaj7 */
    {110.00f, 164.81f, 261.63f, 392.00f}, /* Am7   */
    { 87.31f, 174.61f, 220.00f, 329.63f}, /* Fmaj7 */
    { 98.00f, 146.83f, 246.94f, 293.66f}, /* G6sus */
};
static const float bells[6] = {523.25f, 587.33f, 659.25f, 783.99f, 880.00f, 1046.50f};
static float padF[4], padTarget[4];
static u32 padPh[4], lfoPh;
static int chordIdx, chordTimer, bellTimer = 44100;

static inline float wave(Voice *v) {
    u32 p = v->phase;
    switch (v->wave) {
    case W_TRI: {
        float t = (p >> 8) * (1.0f / 16777216.0f);
        return t < 0.5f ? t * 4 - 1 : 3 - t * 4;
    }
    case W_SQUARE: return (p & 0x80000000) ? 0.55f : -0.55f;
    case W_NOISE:
        noiseState ^= noiseState << 13; noiseState ^= noiseState >> 17; noiseState ^= noiseState << 5;
        return (int)noiseState * (1.0f / 2147483648.0f);
    default: return sinTab[p >> (32 - TAB_BITS)];
    }
}

static void startVoice(Voice *v, float delay, float f0, float f1, float dur, float att, float amp, int w) {
    v->on = 0;
    v->delay = (int)(delay * RATE);
    v->len = (int)(dur * RATE);
    if (v->len < 1) v->len = 1;
    v->att = (int)(att * RATE);
    if (v->att < 1) v->att = 1;
    if (v->att >= v->len) v->att = v->len - 1;
    v->pos = 0;
    v->inc = HZ2INC(f0);
    v->dinc = (int)(((float)HZ2INC(f1) - (float)HZ2INC(f0)) / v->len);
    v->env = 0;
    v->dAtt = 1.0f / v->att;
    v->t = 1.0f;
    v->dDec = 1.0f / (v->len - v->att);
    v->amp = amp;
    v->phase = 0;
    v->wave = w;
    v->on = 1;
}

/* music parameters change slowly: update them once per audio block */
static u32 padInc[4];
static float lfoVal = 1;

static void musicBlock(unsigned int n) {
    chordTimer -= n;
    if (chordTimer <= 0) {
        chordTimer = (int)(RATE * 7);
        chordIdx = (chordIdx + 1) & 3;
        for (int i = 0; i < 4; i++) padTarget[i] = chords[chordIdx][i];
    }
    float k = 0.00003f * n; /* slow glide between chords */
    for (int i = 0; i < 4; i++) {
        padF[i] += (padTarget[i] - padF[i]) * k;
        padInc[i] = HZ2INC(padF[i]);
    }
    lfoPh += HZ2INC(0.13f) * n;
    lfoVal = 0.75f + 0.25f * sinTab[lfoPh >> (32 - TAB_BITS)];
    bellTimer -= n;
    if (bellTimer <= 0) {
        bellTimer = (int)(RATE * (0.9f + (noiseState % 1500) / 1000.0f));
        noiseState = noiseState * 1103515245u + 12345u;
        for (int i = NSFX; i < NV; i++)
            if (!vo[i].on) {
                float f = bells[(noiseState >> 16) % 6];
                startVoice(&vo[i], 0, f, f, 2.4f, 0.004f, 0.03f, W_SINE);
                break;
            }
    }
}

/* ---------------------------------------------------------------- mixer */
static void audioCb(void *buf, unsigned int n, void *user) {
    (void)user;
    s16 *out = (s16 *)buf;
    float target = musicOn ? (duck ? 0.35f : 1.0f) * musicGain : 0.0f;
    float sg = sfxOn ? sfxGain : 0.0f;

    /* nothing playing: silence without touching the synth (saves CPU) */
    int active = 0;
    for (int k = 0; k < NV; k++) active |= vo[k].on;
    if (!active && target < 0.001f && musicVol < 0.001f) {
        memset(out, 0, n * 4);
        return;
    }
    musicBlock(n);
    float mvStep = (target - musicVol) * 0.00004f; /* smooth fade, ~4% of the gap per block */

    for (unsigned int i = 0; i < n; i++) {
        float s = 0, m = 0;
        for (int k = 0; k < NV; k++) {
            Voice *v = &vo[k];
            if (!v->on) continue;
            if (v->delay > 0) { v->delay--; continue; }
            if (v->pos < v->att) v->env += v->dAtt;
            else { v->t -= v->dDec; v->env = v->t * v->t; }
            float x = wave(v) * v->env * v->amp;
            if (k < NSFX) s += x; else m += x;
            v->phase += v->inc;
            v->inc += v->dinc;
            if (++v->pos >= v->len) v->on = 0;
        }
        musicVol += mvStep;
        if (musicVol > 0.001f) {
            float p = 0;
            for (int j = 0; j < 4; j++) {
                padPh[j] += padInc[j];
                p += sinTab[padPh[j] >> (32 - TAB_BITS)] * (j == 0 ? 0.05f : 0.032f);
            }
            m = (m + p * lfoVal) * musicVol;
        } else m = 0;
        float mix = s * sg + m;
        mix = mix / (1.0f + fabsf(mix) * 0.6f); /* soft limiter */
        int v = (int)(mix * 30000.0f);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i * 2] = out[i * 2 + 1] = (s16)v;
    }
}

/* ---------------------------------------------------------------- api */
void audioInit(void) {
    if (inited) return;
    for (int i = 0; i < TAB_SIZE; i++) sinTab[i] = sinf(i * 6.2831853f / TAB_SIZE);
    for (int i = 0; i < 4; i++) padF[i] = padTarget[i] = chords[0][i];
    chordTimer = (int)(RATE * 7);
    memset(vo, 0, sizeof(vo));
    pspAudioInit();
    pspAudioSetChannelCallback(0, audioCb, NULL);
    inited = 1;
}

void audioShutdown(void) {
    if (!inited) return;
    pspAudioSetChannelCallback(0, NULL, NULL);
    pspAudioEnd();
    inited = 0;
}

void audioSetSfx(int on) { sfxOn = on; }
void audioSetMusic(int on) { musicOn = on; }
void audioDuckMusic(int d) { duck = d; }
void audioSetVolume(float s, float m) { sfxGain = s; musicGain = m; }

static Voice *grab(void) {
    Voice *best = &vo[0];
    for (int i = 0; i < NSFX; i++) {
        if (!vo[i].on) return &vo[i];
        if (vo[i].pos > best->pos) best = &vo[i];
    }
    return best;
}

static void note(float delay, float f0, float f1, float dur, float att, float amp, int w) {
    startVoice(grab(), delay, f0, f1, dur, att, amp, w);
}

void sfxPitch(int id, float p) {
    if (!sfxOn || !inited) return;
    switch (id) {
    case S_MOVE:   note(0, 2350 * p, 2150 * p, 0.035f, 0.002f, 0.10f, W_SINE);
                   note(0, 1175 * p, 1100 * p, 0.03f, 0.002f, 0.05f, W_TRI); break;
    case S_EDGE:   note(0, 300, 260, 0.05f, 0.003f, 0.14f, W_TRI); break;
    case S_TAB:    note(0, 1480, 1760, 0.06f, 0.004f, 0.12f, W_SINE); break;
    case S_OK:     note(0, 880, 880, 0.12f, 0.004f, 0.14f, W_SINE);
                   note(0.06f, 1318.5f, 1318.5f, 0.2f, 0.004f, 0.12f, W_SINE); break;
    case S_BACK:   note(0, 1046, 700, 0.09f, 0.004f, 0.12f, W_SINE); break;
    case S_OPEN:   note(0, 520, 880, 0.12f, 0.01f, 0.11f, W_SINE);
                   note(0.03f, 1040, 1760, 0.12f, 0.01f, 0.04f, W_SINE); break;
    case S_CLOSE:  note(0, 880, 520, 0.12f, 0.01f, 0.10f, W_SINE); break;
    case S_ERROR:  note(0, 220, 200, 0.12f, 0.005f, 0.12f, W_SQUARE);
                   note(0.14f, 180, 160, 0.16f, 0.005f, 0.12f, W_SQUARE); break;
    case S_BOOT:   note(0, 261.6f, 261.6f, 2.8f, 0.6f, 0.08f, W_SINE);
                   note(0.05f, 392.0f, 392.0f, 2.7f, 0.6f, 0.07f, W_SINE);
                   note(0.10f, 523.3f, 523.3f, 2.6f, 0.6f, 0.06f, W_SINE);
                   note(0.15f, 659.3f, 659.3f, 2.5f, 0.6f, 0.05f, W_SINE);
                   note(0.7f, 1568.0f, 1568.0f, 1.6f, 0.01f, 0.04f, W_SINE); break;
    case S_LOGIN:  note(0, 659.3f, 659.3f, 0.5f, 0.01f, 0.10f, W_SINE);
                   note(0.08f, 987.8f, 987.8f, 0.6f, 0.01f, 0.09f, W_SINE);
                   note(0.16f, 1318.5f, 1318.5f, 0.9f, 0.01f, 0.07f, W_SINE); break;
    case S_LAUNCH: note(0, 300, 1200, 0.45f, 0.05f, 0.11f, W_SINE);
                   note(0.25f, 523.3f, 523.3f, 1.3f, 0.02f, 0.08f, W_SINE);
                   note(0.25f, 659.3f, 659.3f, 1.3f, 0.02f, 0.07f, W_SINE);
                   note(0.25f, 784.0f, 784.0f, 1.3f, 0.02f, 0.06f, W_SINE); break;
    case S_TOGGLE: note(0, 1760 * p, 1760 * p, 0.05f, 0.002f, 0.10f, W_SINE); break;
    case S_TICK:   note(0, 660, 660, 0.12f, 0.003f, 0.14f, W_SINE); break;
    case S_GO:     note(0, 1320, 1320, 0.3f, 0.003f, 0.14f, W_SINE); break;
    case S_EAT:    note(0, 660 * p, 1320 * p, 0.08f, 0.003f, 0.15f, W_TRI); break;
    case S_HIT:    note(0, 900 * p, 860 * p, 0.05f, 0.002f, 0.07f, W_SQUARE); break;
    case S_PADDLE: note(0, 440, 430, 0.05f, 0.002f, 0.14f, W_TRI); break;
    case S_LOSE:   note(0, 440, 110, 0.5f, 0.005f, 0.11f, W_SQUARE);
                   note(0, 200, 60, 0.4f, 0.005f, 0.08f, W_NOISE); break;
    case S_WIN:    for (int i = 0; i < 4; i++) {
                       static const float a[4] = {523.3f, 659.3f, 784.0f, 1046.5f};
                       note(i * 0.1f, a[i], a[i], 0.5f, 0.005f, 0.11f, W_SINE);
                   } break;
    case S_PAUSE:  note(0, 700, 1000, 0.08f, 0.005f, 0.10f, W_SINE); break;
    }
}
void sfx(int id) { sfxPitch(id, 1.0f); }
