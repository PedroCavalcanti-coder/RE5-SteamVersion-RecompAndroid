/*
 * trace_packet.h - Estrutura binária de 128 bytes para Deterministic Replay & Trace Diff
 * compartilhada entre o proxy do jogo original (tools/re5_logger) e o recompilado (runtime).
 */
#ifndef RE5_TRACE_PACKET_H
#define RE5_TRACE_PACKET_H

#include <stdint.h>
#include <stddef.h>

#define RE5_TRACE_MAGIC 0x52453554u /* "RE5T" */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          /* 0x52453554 ("RE5T") */
    uint32_t frameIndex;     /* Contador de quadros do TAS / gravação */
    uint32_t hookSite;       /* 1 = Pre-0x00B72FA0, 2 = Post-0x00B72FA0 */
    uint32_t characterId;    /* 0 = Chris, 1 = Sheva */

    /* Estado de Tempo e Input */
    float    deltaTime;      /* Delta time da engine / cUnit (+0x10) */
    float    inputStickX;    /* Input analógico X (-1.0f .. 1.0f) */
    float    inputStickY;    /* Input analógico Y (-1.0f .. 1.0f) */

    /* Transformação e Movimento (cPlayer / cModel) */
    float    pos[3];         /* mPos (X, Y, Z) (+0x30, +0x34, +0x38) */
    float    oldPos[3];      /* mOldPos / alvo (X, Y, Z) (+0x1170, +0x1174, +0x1178) */
    float    moveVec[3];     /* Vetor de deslocamento do quadro (+0x1400, +0x1404, +0x1408) */

    /* Estado de Colisão Estática (SBC) */
    uint32_t collisionFlags; /* Flags de chão/parede/escada (+0x2D7C) */
    uint32_t filterMask;     /* Máscara de filtro enviada ao SBC1 */
    float    stepHeight;     /* Limite de degrau (mStepHeight) */
    float    slopeCos;       /* Cosseno de mSlopeDegree */
    float    wallCos;        /* Cosseno de mWallDegree */

    /* Hash rápido da struct do Player (primeiros 0x2000 bytes) */
    uint32_t playerStructCRC32;

    /* Padding reservado para totalizar exatamente 128 bytes */
    uint8_t  reserved[40];
} FrameTracePacket;
#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(FrameTracePacket) == 128, "FrameTracePacket must be exactly 128 bytes");
#else
_Static_assert(sizeof(FrameTracePacket) == 128, "FrameTracePacket must be exactly 128 bytes");
#endif

/* CRC32 padrão IEEE 802.3 (polinômio 0xEDB88320) */
static inline uint32_t re5_trace_crc32(const void *data, size_t length)
{
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

#endif /* RE5_TRACE_PACKET_H */
