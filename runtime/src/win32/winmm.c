/*
 * winmm.c - relógio multimídia (timeGetTime) e resolução do timer.
 *
 * timeGetTime usa o mesmo relógio do GetTickCount (milissegundos desde um
 * ponto fixo, com a volta de 2^32 do Windows). timeBeginPeriod/EndPeriod pedem
 * resolução de timer ao sistema (1..1000 ms; fora disso TIMERR_NOCANDO). No
 * Windows o pedido vai ao host: sem ele um Sleep(1) do runtime (esperas do HLE,
 * seção crítica disputada) dura ~15,6 ms - medido no RE5, a 7 fps com 32% do
 * tempo sem nenhuma thread rodando. No POSIX o nanosleep já é preciso.
 */
#include "kernel32_internal.h"
#include "re5/runtime.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
#include <mmsystem.h>
#endif

#ifndef TIMERR_NOERROR
#define TIMERR_NOERROR 0u
#endif
#ifndef TIMERR_NOCANDO
#define TIMERR_NOCANDO 97u
#endif

static void w_timeGetTime(CPU *c) { c->eax = re5_det_is_enabled() ? re5_det_tick() : k32_tick_ms(); }

static void w_timeBeginPeriod(CPU *c)
{
    uint32_t ms = re5_arg(c, 0);
    c->eax = ms >= 1u && ms <= 1000u ? TIMERR_NOERROR : TIMERR_NOCANDO;
#if defined(RE5_HOST_WINDOWS)
    if (c->eax == TIMERR_NOERROR) timeBeginPeriod(ms);
#endif
}

static void w_timeEndPeriod(CPU *c)
{
    uint32_t ms = re5_arg(c, 0);
    c->eax = ms >= 1u && ms <= 1000u ? TIMERR_NOERROR : TIMERR_NOCANDO;
#if defined(RE5_HOST_WINDOWS)
    if (c->eax == TIMERR_NOERROR) timeEndPeriod(ms);
#endif
}

static const re5_import_def k_defs[] = {
    {"timeBeginPeriod", w_timeBeginPeriod, 4, 0},
    {"timeEndPeriod",   w_timeEndPeriod,   4, 0},
    {"timeGetTime",     w_timeGetTime, 0, 0, RE5_THUNK_NOLOCK},
    {0},
};

const re5_import_module re5_mod_winmm = {"winmm.dll", k_defs};
