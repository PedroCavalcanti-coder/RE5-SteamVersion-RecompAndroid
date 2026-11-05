/*
 * perf_hint.h - núcleos grandes e dica de desempenho do sistema (fase A2 do plano mestre).
 *
 * Num celular big.LITTLE (o Galaxy A54: 4x A78 + 4x A55) o escalonador põe as threads do jogo
 * onde quer, e o governador sobe a frequência pela carga média, que num jogo travado pela
 * espera entre threads parece baixa: 20% de CPU e 20 fps (usuário, 2026-11-05). Aqui:
 * - a thread que chama Present (a principal) e a que desenha (a de render) ficam nos núcleos
 *   fora do cluster mais lento (cpuinfo_max_freq, sem nomes fixos de cluster);
 * - uma sessão ADPF (APerformanceHint, Android 13+) com as duas recebe a duração de cada quadro
 *   contra o alvo, e o sistema ajusta núcleo e frequência para cumprir.
 * Fora do Android tudo vira nada. RE5_PERF_HINT=0 desliga.
 */
#ifndef RE5_PERF_HINT_H
#define RE5_PERF_HINT_H

#include <stdint.h>

/* A cada Present (thread que apresenta). */
void re5_perf_note_present(void);
/* Em um método de desenho frequente (thread de render). Barato depois da primeira vez. */
void re5_perf_note_render(void);

/* Máscara dos núcleos acima do cluster mais lento (cpuinfo_max_freq > o menor), lendo <sysroot>/cpu<N>/cpufreq/cpuinfo_max_freq
 * (sysroot normal: /sys/devices/system/cpu). 0 se todos iguais ou ilegíveis (sem o que separar). */
uint64_t re5_perf_big_mask(const char *sysroot);

#endif
