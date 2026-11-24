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

/* A cada Present (thread que apresenta). idle_ns: quanto do quadro foi o limitador de FPS
 * dormindo (RE5_FPS_CAP), que não é trabalho e sai da duração informada ao ADPF.
 * present_ns: quanto a thread ficou dentro do Present do host. No celular é espera pela GPU (o
 * vkQueuePresentKHR do Mali segura o quadro até a GPU acabar, STATUS 11/11): com o
 * APerformanceHint_reportActualWorkDuration2 (Android 15+) vai como duração de GPU, para o
 * sistema subir o clock da GPU que o governador deixava em 221 MHz. */
void re5_perf_note_present(uint64_t idle_ns, uint64_t present_ns);

/* Divide o quadro para o ADPF: total = intervalo - limitador (>= 1); gpu = a espera no Present,
 * limitada ao total; cpu = total - gpu (>= 1). */
void re5_perf_split(int64_t interval_ns, uint64_t idle_ns, uint64_t present_ns, int64_t *total, int64_t *cpu,
                    int64_t *gpu);
/* Em um método de desenho frequente (thread de render). Barato depois da primeira vez. */
void re5_perf_note_render(void);

/* Máscara dos núcleos acima do cluster mais lento (cpuinfo_max_freq > o menor), lendo <sysroot>/cpu<N>/cpufreq/cpuinfo_max_freq
 * (sysroot normal: /sys/devices/system/cpu). 0 se todos iguais ou ilegíveis (sem o que separar). */
uint64_t re5_perf_big_mask(const char *sysroot);

/* Prender a thread de render nos núcleos grandes? Só com 3 ou mais (A54: 4 A78); com 2 (Dimensity
 * 7030: 2 A78) ela disputava os dois com a principal, que ficava 16% do tempo pronta sem núcleo
 * (Perfetto, 20/11/2026). env = RE5_PIN_RENDER ("0" nunca, "1" sempre, NULL/"" automático). */
int re5_perf_pin_render(uint64_t big_mask, const char *env);

/* No começo de toda thread do host criada pelo runtime: volta à máscara de todos os núcleos. Sem
 * isso a thread herda a afinidade de quem a criou, e os workers do jogo, criados pela principal já
 * presa, ficavam todos nos núcleos grandes (A54: 4 pequenos ociosos). RE5_WORKER_BIG=1 mantém a
 * herança. Fora do Android não faz nada. */
void re5_perf_thread_start(void);

/* Alvo do ADPF em ns a partir do RE5_FPS_CAP (NULL = sem limite). Com limite de 30 a 240 fps,
 * o período dele; sem limite (o padrão do launcher), 60 fps. Antes, sem limite, o alvo era
 * 30 fps: o sistema via os quadros dentro do alvo e BAIXAVA o clock, segurando o jogo perto de
 * 30 fps com a CPU ociosa (usuário, 2026-11-06: "não usa muita CPU e GPU, mesmo precisando"). */
int64_t re5_perf_target_ns(const char *fps_cap);

/* Duração de trabalho de um quadro: intervalo entre Present menos o tempo dormido no limitador
 * (nunca abaixo de 1 ns). */
int64_t re5_perf_work_ns(int64_t interval_ns, uint64_t idle_ns);

#endif
