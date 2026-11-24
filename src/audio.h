/* Tiny real-time synthesizer: UI sounds + ambient background music.
   Nothing is loaded from files - every sound is generated. */
#pragma once

enum {
    S_MOVE, S_EDGE, S_TAB, S_OK, S_BACK, S_OPEN, S_CLOSE, S_ERROR, S_BOOT, S_LOGIN,
    S_LAUNCH, S_TOGGLE, S_TICK, S_GO, S_EAT, S_HIT, S_PADDLE, S_LOSE, S_WIN, S_PAUSE,
    S_COUNT
};

void audioInit(void);
void audioShutdown(void);
void sfx(int id);
void sfxPitch(int id, float mul);
void audioSetSfx(int on);
void audioSetMusic(int on);
void audioDuckMusic(int duck); /* quieter music during launch */
void audioSetVolume(float sfxVol, float musicVol); /* 0..1 */
