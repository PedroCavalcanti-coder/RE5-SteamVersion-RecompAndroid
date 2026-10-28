/*
 * thunks.h - endereços do guest que pertencem ao host.
 *
 * Toda função "de fora" que o jogo chama - imports do Win32, métodos COM de
 * objetos do host (IDirect3DDevice9, IXAudio2...), callbacks - recebe um VA
 * na faixa de thunks. O jogo guarda esse VA na IAT ou numa vtable como
 * guardaria um endereço real; quando o código gerado faz dispatch() para ele,
 * o runtime chama o handler do host.
 *
 * Convenção do handler: no momento da chamada c->esp aponta para o endereço de
 * retorno, e os argumentos estão em c->esp+4, +8, ... (use re5_arg). O handler
 * escreve o resultado em c->eax (e c->edx para 64 bits). Quem remove o
 * endereço de retorno e `arg_bytes` de argumentos da pilha é re5_thunk_invoke,
 * nunca o handler: a contagem de stdcall vem de um só lugar.
 *   stdcall (Win32, COM): arg_bytes = 4 * número de argumentos (COM conta o this)
 *   cdecl (varargs, CRT): arg_bytes = 0
 */
#ifndef RE5_THUNKS_H
#define RE5_THUNKS_H

#include "cpu.h"

#define RE5_THUNK_BASE   0xFFFC0000u
#define RE5_THUNK_LIMIT  0xFFFF0000u
#define RE5_THUNK_STRIDE 8u
#define RE5_THUNK_MAX    ((RE5_THUNK_LIMIT - RE5_THUNK_BASE) / RE5_THUNK_STRIDE)

/* Endereço de retorno usado quando o host chama código do guest. O primeiro
 * thunk; se algo fizer dispatch para ele, é um bug de pilha. */
#define RE5_RETURN_TO_HOST RE5_THUNK_BASE

typedef void (*re5_host_fn)(CPU *c);

typedef struct {
    re5_host_fn fn;
    const char *name;      /* "kernel32.dll!CreateFileA", "IDirect3DDevice9::Present"... */
    uint32_t    arg_bytes;
    void       *user;      /* dado do handler (ex.: o objeto host de um método COM) */
    uint32_t    flags;     /* RE5_THUNK_* */
} re5_thunk;

/* Handler que não precisa do lock do HLE (RE5_GUEST_LOCK=hle): só mexe no que
 * é da própria thread (TEB, CPU) ou usa atômicos do host. */
#define RE5_THUNK_NOLOCK 0x1u
/* Sem o lock do HLE só quando a chave liga (RE5_HEAP_NOLOCK=1, fase P1 do
 * PLANO_MESTRE_OTIMIZACAO): o handler tem mutex próprio. Os métodos D3D9 ficam de fora:
 * a ponte tem estado compartilhado (hash de objetos, travas, sombras) e um mutex dela
 * poderia travar contra o lock do HLE quando um callback do guest chama o D3D9. */
#define RE5_THUNK_NOLOCK_HEAP 0x2u

int  re5_thunks_init(void);
uint32_t re5_thunk_new(const char *name, re5_host_fn fn, uint32_t arg_bytes, void *user);
void re5_thunk_set_flags(uint32_t va, uint32_t flags);
const re5_thunk *re5_thunk_get(uint32_t va);
uint32_t re5_thunk_count(void);
void re5_thunk_invoke(CPU *c, uint32_t va);

/* Thunk que o handler corrente está atendendo (para handlers genéricos). */
const re5_thunk *re5_thunk_current(void);

static inline int re5_is_thunk(uint32_t va)
{
    return va >= RE5_THUNK_BASE && va < RE5_THUNK_LIMIT;
}

/* i-ésimo argumento (0 = primeiro) de uma chamada stdcall/cdecl. */
static inline uint32_t re5_arg(CPU *c, int i) { return rd32(c->esp + 4u + 4u * (uint32_t)i); }

#endif /* RE5_THUNKS_H */
