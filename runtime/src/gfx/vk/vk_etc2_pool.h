#ifndef RE5_VK_ETC2_POOL_H
#define RE5_VK_ETC2_POOL_H
/* Recodificação DXT -> ETC2/EAC de uma superfície em paralelo (vk_etc2_pool.c). Mesmo resultado de
 * re5_dxt_to_etc2_surface; bloqueia até a superfície inteira estar pronta. */
#include <stdint.h>

void re5vk_etc2_surface(int mode, uint32_t w, uint32_t h, const uint8_t *src, uint32_t src_pitch, uint8_t *dst,
                        uint32_t dst_pitch);
/* Threads auxiliares (sobe o conjunto na primeira chamada). */
int re5vk_etc2_threads(void);

#endif
