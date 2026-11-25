/*
 * vk2_dxso.h - tradutor de shaders Direct3D 9 SM3 (vs_3_0 / ps_3_0) para SPIR-V, do backend re5vk.
 *
 * Interface de recursos que o SPIR-V gerado espera (a mesma para todo shader, um layout de
 * pipeline só; ver docs/PROJETO_VULKAN_NATIVO.md):
 *   set 0, binding 0   uniform buffer do VS: vec4 c[256]; ivec4 i[16]; uint b;      (dinâmico)
 *   set 0, binding 1   uniform buffer do PS: vec4 c[224]; ivec4 i[16]; uint b; float alpha_ref
 *   set 0, binding 2+n sampler combinado s<n> do PS (n < 16)
 *   set 0, binding 18+n sampler combinado s<n> do VS (n < 4)
 * Entradas do VS: location = número do registrador v<n>. Saídas do VS e entradas do PS:
 * location pelo uso (vk2_dxso_location), para os dois lados casarem pela semântica.
 */
#ifndef RE5VK2_DXSO_H
#define RE5VK2_DXSO_H

#include <stddef.h>
#include <stdint.h>

#define RE5VK2_VS_UBO_SIZE 4368u   /* 256*16 + 16*16 + 16 */
#define RE5VK2_PS_UBO_SIZE 3856u   /* 224*16 + 16*16 + 16 */
#define RE5VK2_VS_UBO_I    4096u   /* offset de i[] no UBO do VS */
#define RE5VK2_VS_UBO_B    4352u
#define RE5VK2_PS_UBO_I    3584u
#define RE5VK2_PS_UBO_B    3840u
#define RE5VK2_PS_UBO_AREF 3844u

#define RE5VK2_BIND_VS_UBO   0u
#define RE5VK2_BIND_PS_UBO   1u
#define RE5VK2_BIND_PS_SAMP  2u
#define RE5VK2_BIND_VS_SAMP  18u
#define RE5VK2_NBINDINGS     22u

enum { RE5VK2_TEX_2D = 2, RE5VK2_TEX_CUBE = 3, RE5VK2_TEX_3D = 4 };

/* Variante do shader (a chave do cache de módulos junto com o bytecode). */
typedef struct {
    uint16_t depth_samplers;   /* PS: samplers ligados a textura de profundidade (comparação com .z) */
    /* VS: entradas v<n> que chegam como inteiro sem sinal / com sinal e viram float no shader.
     * Para GPUs sem os formatos de vértice *SCALED (Adreno): UBYTE4, SHORT2/4, UDEC3. */
    uint16_t in_uint, in_sint;
    uint16_t in_udec3;         /* VS: entrada v<n> que chega como uint32 (R32_UINT) e é desempacotada (10-10-10-2) */
    uint8_t  alpha_func;       /* PS: D3DCMPFUNC do alpha test; 0 ou 8 (ALWAYS) = sem teste */
    uint8_t  pad_;             /* sem enchimento implícito: a variante entra no hash da pipeline */
} vk2_dxso_variant;

typedef struct {
    uint32_t *spv;              /* malloc; o chamador libera */
    size_t    nwords;
    int       pixel;
    /* VS: semântica de cada registrador de entrada v<n> */
    uint16_t  inputs;           /* bits dos v<n> declarados */
    uint8_t   in_usage[16], in_index[16];
    /* samplers declarados (bits) e o tipo de cada um (RE5VK2_TEX_*) */
    uint16_t  samplers;
    uint8_t   sampler_type[16];
    uint8_t   writes_depth;
    /* Constantes que o shader lê do UBO: c[0 .. const_f-1] (o banco inteiro com endereçamento
     * relativo) e se lê i[] / b. O prepare() do device copia só isso a cada draw. */
    uint16_t  const_f;
    uint8_t   uses_i, uses_b;
    uint32_t  locs;             /* VS: locations que o shader escreve; PS: as que lê (vk2_dxso_location). A ligação VS->PS confere os dois */
    uint8_t   uses_rel;
    uint8_t   uses_kill;        /* PS descarta pixel (texkill ou alpha test) */         /* lê c[] por endereçamento relativo (skinning: c[a0.x + n]) */
} vk2_shader;

/* Location de uma semântica D3DDECLUSAGE (uso, índice) entre o VS e o PS. */
uint32_t vk2_dxso_location(uint32_t usage, uint32_t index);

/* Traduz; 0 = ok. Erro: -1 e a mensagem em err (instrução sem tradução, com o deslocamento). */
int vk2_dxso_compile(const uint32_t *code, size_t ndwords, const vk2_dxso_variant *var, vk2_shader *out,
                       char *err, size_t errsz);

#endif /* RE5VK2_DXSO_H */
