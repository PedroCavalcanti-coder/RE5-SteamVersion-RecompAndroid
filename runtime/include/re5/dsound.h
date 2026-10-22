/*
 * dsound.h - DirectSound 8 sobre o áudio do SDL2 (runtime/src/win32/dsound.c).
 */
#ifndef RE5_DSOUND_H
#define RE5_DSOUND_H

#include <stdint.h>

#define RE5_DSOUND_RATE 48000   /* saída do mixer: 48 kHz, estéreo, float */

/* Mistura `frames` quadros estéreo intercalados em `out` e avança os cursores
 * dos buffers tocando. A thread de áudio do SDL chama isto; sem dispositivo
 * (RE5_HEADLESS, testes) quem chama é o teste. */
void re5_dsound_mix(float *out, uint32_t frames);

#endif /* RE5_DSOUND_H */
