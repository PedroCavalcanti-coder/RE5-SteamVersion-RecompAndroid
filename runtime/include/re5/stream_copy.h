/*
 * stream_copy.h - cópia grande de buffer de vértice/índice (sombra do guest -> buffer do host) no Unlock.
 *
 * RE5_COPY_NT=1 (ARM64): stores não-temporais (stnp) para cópias >= 256 KB: a escrita não aloca linha no L1/L2, que é
 * o que o Unlock de ~5 MB x 49 por segundo (~245 MB/s) poluía nos Cortex-A78/A55. Padrão desligado até medir no aparelho
 * (RE5_FPS_LOG mostra o tempo das cópias de Unlock). Fora do ARM64, ou abaixo do limite, é o memcpy comum.
 */
#ifndef RE5_STREAM_COPY_H
#define RE5_STREAM_COPY_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int re5_copy_nt_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5_COPY_NT");
        on = e && *e == '1';
    }
    return on;
}

static inline void re5_copy_big(void *dst, const void *src, size_t n)
{
#if defined(__aarch64__)
    if (n >= (256u << 10) && re5_copy_nt_enabled()) {
        uint8_t *d = (uint8_t *)dst;
        const uint8_t *s = (const uint8_t *)src;
        size_t head = (size_t)(-(uintptr_t)d & 15u);   /* alinha o destino a 16 bytes */
        if (head) {
            memcpy(d, s, head);
            d += head;
            s += head;
            n -= head;
        }
        size_t blk = n & ~(size_t)63u;
        for (size_t i = 0; i < blk; i += 64)
            __asm__ volatile("ldp q0, q1, [%1]\n\t"
                             "ldp q2, q3, [%1, #32]\n\t"
                             "stnp q0, q1, [%0]\n\t"
                             "stnp q2, q3, [%0, #32]\n\t"
                             :
                             : "r"(d + i), "r"(s + i)
                             : "v0", "v1", "v2", "v3", "memory");
        if (n - blk) memcpy(d + blk, s + blk, n - blk);
        return;
    }
#endif
    memcpy(dst, src, n);
}

#endif /* RE5_STREAM_COPY_H */
