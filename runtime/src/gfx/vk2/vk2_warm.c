/*
 * vk2_warm.c - cache de shaders e pré-compilação de pipelines do vk2.
 *
 * O que custa no primeiro draw de cada combinação é traduzir o shader (DXSO -> SPIR-V, barato) e, bem
 * mais caro, o driver compilar o pipeline (vkCreateGraphicsPipelines, dezenas de ms no Mali). Isto
 * grava num arquivo ("manifesto", vk2.shaders, na pasta de cache do app) tudo o que é preciso para refazer
 * cada pipeline que o jogo criou: bytecode dos shaders, declaração de vértices e a chave de estado. No
 * arranque seguinte, threads auxiliares leem o manifesto e criam os pipelines ANTES de o jogo pedi-los
 * (o VkPipelineCache em disco, vk2.pcache, deixa o driver pular a compilação de verdade).
 *
 *   RE5VK2_PRECOMPILE   1 (padrão) = em segundo plano, durante o carregamento do jogo;
 *                       wait = o primeiro Present espera acabar (tela de carregamento "compila tudo antes de entrar");
 *                       0 = desligado (nem grava);
 *   RE5VK2_PRECOMPILE_THREADS  threads (padrão: núcleos - 2, de 1 a 4);
 *   RE5VK2_PRECOMPILE_TIMEOUT  segundos máximos de espera no modo wait (padrão 180);
 *   RE5VK2_CACHE_CLEAR  1 = apaga manifesto e cache de pipelines antes de começar (o botão "Apagar caches" do app
 *                       apaga a pasta inteira);
 *   RE5VK2_CACHE_DIR    pasta dos arquivos (Android: cache do app).
 *
 * O manifesto só vale para a mesma GPU/driver e a mesma versão da chave (cabeçalho); senão recomeça.
 * Primeira execução = aprende; da segunda em diante, compila tudo antes. Formato: cabeçalho + registros
 * [tipo u32][tamanho u32][carga]; um registro cortado no fim (processo morto) é descartado.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "vk2_core.h"
#include "re5/thread.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define CH "vk2"
#define MAN_MAGIC 0x4D324B56u   /* "VK2M" */
#define MAN_VERSION 1u
enum { REC_SHADER = 1, REC_DECL = 2, REC_PIPE = 3 };

typedef struct { uint32_t magic, version, keysz, vendor, device, driver; uint8_t uuid[VK_UUID_SIZE]; } man_header;

/* ---- pasta e arquivos ---- */

const char *vk2_cache_dir(void)
{
    static char dir[1024];
    static int done;
    if (done) return dir;
    done = 1;
    const char *e = getenv("RE5VK2_CACHE_DIR");
    if (e && *e) {
        snprintf(dir, sizeof dir, "%s", e);
    } else {
#if defined(__ANDROID__)
        const char *base = SDL_AndroidGetInternalStoragePath();
        snprintf(dir, sizeof dir, "%s", base ? base : ".");
#else
        snprintf(dir, sizeof dir, ".");
#endif
    }
    return dir;
}

static void path_of(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", vk2_cache_dir(), name);
}

/* Antes de abrir o cache de pipelines (layout_create). */
void vk2_cache_init(void)
{
    const char *c = getenv("RE5VK2_CACHE_CLEAR");
    if (!(c && *c == '1')) return;
    static const char *const names[] = {"vk2.pcache", "vk2.pcache.tmp", "vk2.shaders"};
    char p[1100];
    int n = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        path_of(p, sizeof p, names[i]);
        n += remove(p) == 0;
    }
    RE5_INFO(CH, "RE5VK2_CACHE_CLEAR=1: %d arquivo(s) de cache apagado(s) em %s", n, vk2_cache_dir());
}

/* ---- conjunto de chaves de 64 bits (o que já está no arquivo) ---- */

typedef struct { uint64_t *k; uint32_t n, cap; } set64;

static int set_has(const set64 *s, uint64_t key)
{
    if (!s->cap) return 0;
    for (uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) {
        if (!s->k[i]) return 0;
        if (s->k[i] == key) return 1;
    }
}

static void set_add(set64 *s, uint64_t key)
{
    key |= 1;   /* 0 = vaga */
    if (set_has(s, key)) return;
    if ((s->n + 1) * 2 > s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 256;
        uint64_t *nk = calloc(nc, sizeof *nk);
        if (!nk) return;
        for (uint32_t i = 0; i < s->cap; i++)
            if (s->k[i]) {
                uint32_t j = (uint32_t)(s->k[i] * 0x9E3779B97F4A7C15ull >> 40) & (nc - 1);
                while (nk[j]) j = (j + 1) & (nc - 1);
                nk[j] = s->k[i];
            }
        free(s->k);
        s->k = nk;
        s->cap = nc;
    }
    uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (s->cap - 1);
    while (s->k[i]) i = (i + 1) & (s->cap - 1);
    s->k[i] = key;
    s->n++;
}

static uint64_t fnv64(const void *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    return h;
}

/* ---- estado ---- */

static struct {
    int mode;                  /* 0 desligado, 1 segundo plano, 2 espera */
    re5_mutex mu;
    FILE *f;
    set64 seen;                /* (tipo, hash) de cada registro já gravado */
    /* pré-compilação */
    uint8_t *buf;              /* arquivo inteiro (liberado pelo último worker) */
    uint32_t *pipe_off;        /* deslocamentos dos registros de pipeline em buf */
    uint32_t npipe;
    struct ent { uint32_t hash, off, len; } *shaders[2], *decls;
    uint32_t nsh[2], ndecl;
    volatile uint32_t next, done, workers_left, total;
    vk_device *d;
    int started;
    uint32_t timeout_s;
} W;

static uint64_t seen_key(int type, uint64_t hash) { return (uint64_t)type << 40 ^ hash; }

static void write_rec(int type, const void *a, size_t na, const void *b, size_t nb, const void *c, size_t nc)
{
    uint32_t hdr[2] = {(uint32_t)type, (uint32_t)(na + nb + nc)};
    fwrite(hdr, sizeof hdr, 1, W.f);
    if (na) fwrite(a, 1, na, W.f);
    if (nb) fwrite(b, 1, nb, W.f);
    if (nc) fwrite(c, 1, nc, W.f);
}

/* Grava (uma vez cada) os shaders e a declaração de um pipeline novo e a chave dele. */
void vk2_warm_record(vk_device *d, const void *key, uint32_t keysz, const vk_shader *vs, const vk_shader *ps,
                     const vk_decl *decl)
{
    (void)d;
    if (!W.mode || !W.f) return;
    re5_mutex_lock(&W.mu);
    const vk_shader *sh[2] = {vs, ps};
    for (int i = 0; i < 2; i++) {
        uint64_t sk = seen_key(REC_SHADER, (uint64_t)i << 32 | sh[i]->hash);
        if (set_has(&W.seen, sk | 1)) continue;
        uint32_t head[3] = {(uint32_t)i, sh[i]->hash, sh[i]->ndw};
        write_rec(REC_SHADER, head, sizeof head, sh[i]->code, sh[i]->ndw * 4u, NULL, 0);
        set_add(&W.seen, sk);
    }
    uint32_t dh = 0;
    if (decl) {
        dh = (uint32_t)fnv64(decl->el, decl->n * sizeof decl->el[0], 0xcbf29ce484222325ull) | 1u;
        if (!set_has(&W.seen, seen_key(REC_DECL, dh) | 1)) {
            uint32_t head[2] = {dh, decl->n};
            write_rec(REC_DECL, head, sizeof head, decl->el, decl->n * sizeof decl->el[0], NULL, 0);
            set_add(&W.seen, seen_key(REC_DECL, dh));
        }
    }
    uint64_t pk = fnv64(key, keysz, 0xcbf29ce484222325ull);
    if (!set_has(&W.seen, seen_key(REC_PIPE, pk) | 1)) {
        uint32_t head[3] = {vs->hash, ps->hash, dh};
        write_rec(REC_PIPE, head, sizeof head, key, keysz, NULL, 0);
        set_add(&W.seen, seen_key(REC_PIPE, pk));
        fflush(W.f);   /* o Android mata o processo sem aviso */
    }
    re5_mutex_unlock(&W.mu);
}

/* ---- pré-compilação ---- */

static const struct ent *find(const struct ent *v, uint32_t n, uint32_t hash)
{
    for (uint32_t i = 0; i < n; i++)
        if (v[i].hash == hash) return &v[i];
    return NULL;
}

/* A pré-compilação roda enquanto o jogo carrega: prioridade baixa para não tirar CPU dos workers do jogo
 * (os mesmos núcleos grandes). O carregamento até a 1ª cena 3D no A54 foi 31,6 s no vk2 contra 24,0 s na DXVK. */
static void lower_priority(void)
{
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__) || defined(__ANDROID__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
}

static void warm_worker(void *arg)
{
    (void)arg;
    lower_priority();
    for (;;) {
        uint32_t i = __atomic_fetch_add(&W.next, 1, __ATOMIC_RELAXED);
        if (i >= W.npipe) break;
        const uint8_t *p = W.buf + W.pipe_off[i];   /* aponta a carga: [vs u32][ps u32][decl u32][chave] */
        uint32_t len, vs_h, ps_h, d_h;
        memcpy(&len, p - 4, 4);
        memcpy(&vs_h, p, 4);
        memcpy(&ps_h, p + 4, 4);
        memcpy(&d_h, p + 8, 4);
        const struct ent *vs = find(W.shaders[0], W.nsh[0], vs_h), *ps = find(W.shaders[1], W.nsh[1], ps_h);
        const struct ent *de = d_h ? find(W.decls, W.ndecl, d_h) : NULL;
        if (vs && ps && (!d_h || de)) {
            const uint8_t *vp = W.buf + vs->off, *pp = W.buf + ps->off;   /* carga do shader: [pixel][hash][ndw][dwords] */
            uint32_t nvs, nps, nel = 0;
            memcpy(&nvs, vp + 8, 4);
            memcpy(&nps, pp + 8, 4);
            const D3DVERTEXELEMENT9 *el = NULL;
            if (de) {
                memcpy(&nel, W.buf + de->off + 4, 4);
                el = (const D3DVERTEXELEMENT9 *)(W.buf + de->off + 8);
            }
            vk2_draw_warm_one(W.d, p + 12, len - 12, (const uint32_t *)(vp + 12), nvs, (const uint32_t *)(pp + 12), nps,
                              el, nel);
        }
        __atomic_add_fetch(&W.done, 1, __ATOMIC_RELAXED);
    }
    if (__atomic_sub_fetch(&W.workers_left, 1, __ATOMIC_ACQ_REL) == 0) {
        RE5_INFO(CH, "pré-compilação: %u pipelines prontos antes do jogo pedir (%u pipelines criados no total)", W.done,
                 W.d->npipes);
        free(W.buf);
        free(W.pipe_off);
        free(W.shaders[0]);
        free(W.shaders[1]);
        free(W.decls);
        W.buf = NULL;
    }
}

static int read_file(const char *path, uint8_t **out, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = sz > 0 && sz < (512L << 20) ? malloc((size_t)sz) : NULL;
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = b;
    *n = (size_t)sz;
    return 0;
}

void vk2_warm_start(vk_device *d)
{
    const char *e = getenv("RE5VK2_PRECOMPILE");
    W.mode = e && *e == '0' ? 0 : (e && !strcmp(e, "wait")) ? 2 : 1;
    if (!W.mode) {
        RE5_INFO(CH, "pré-compilação de pipelines desligada (RE5VK2_PRECOMPILE=0)");
        return;
    }
    re5_mutex_init(&W.mu);
    W.d = d;
    const char *t = getenv("RE5VK2_PRECOMPILE_TIMEOUT");
    W.timeout_s = t && atoi(t) > 0 ? (uint32_t)atoi(t) : 180;
    char path[1100];
    path_of(path, sizeof path, "vk2.shaders");

    man_header want;
    memset(&want, 0, sizeof want);
    want.magic = MAN_MAGIC;
    want.version = MAN_VERSION;
    want.keysz = 0;   /* preenchido pelo primeiro registro de pipeline: a chave do desenho muda com o código */
    want.vendor = d->props.vendorID;
    want.device = d->props.deviceID;
    want.driver = d->props.driverVersion;
    memcpy(want.uuid, d->props.pipelineCacheUUID, VK_UUID_SIZE);

    uint8_t *buf = NULL;
    size_t n = 0, valid = 0;
    int keep = 0;
    if (read_file(path, &buf, &n) == 0 && n >= sizeof(man_header)) {
        man_header h;
        memcpy(&h, buf, sizeof h);
        want.keysz = h.keysz;
        if (h.magic == MAN_MAGIC && h.version == MAN_VERSION && !memcmp(&h.vendor, &want.vendor, sizeof h - offsetof(man_header, vendor)) &&
            h.keysz == vk2_pipe_key_size())
            keep = 1;
        else
            RE5_INFO(CH, "manifesto de shaders é de outra GPU/driver/versão: recomeçando");
    }
    want.keysz = (uint32_t)vk2_pipe_key_size();

    uint32_t cap = 0;
    if (keep) {
        valid = sizeof(man_header);
        /* varre os registros: guarda onde cada um começa */
        for (size_t o = sizeof(man_header); o + 8 <= n;) {
            uint32_t type, len;
            memcpy(&type, buf + o, 4);
            memcpy(&len, buf + o + 4, 4);
            if (o + 8 + len > n || type < REC_SHADER || type > REC_PIPE) break;
            o += 8 + len;
            valid = o;
            if (type == REC_PIPE) cap++;
        }
        W.pipe_off = malloc((cap ? cap : 1) * sizeof *W.pipe_off);
        W.shaders[0] = malloc(4096 * sizeof(struct ent));
        W.shaders[1] = malloc(4096 * sizeof(struct ent));
        W.decls = malloc(4096 * sizeof(struct ent));
        uint32_t cs[2] = {4096, 4096}, cd = 4096;
        for (size_t o = sizeof(man_header); o < valid;) {
            uint32_t type, len;
            memcpy(&type, buf + o, 4);
            memcpy(&len, buf + o + 4, 4);
            const uint8_t *pl = buf + o + 8;
            if (type == REC_SHADER) {
                uint32_t px, h;
                memcpy(&px, pl, 4);
                memcpy(&h, pl + 4, 4);
                px &= 1;
                if (W.nsh[px] == cs[px]) {
                    cs[px] *= 2;
                    W.shaders[px] = realloc(W.shaders[px], cs[px] * sizeof(struct ent));
                }
                W.shaders[px][W.nsh[px]++] = (struct ent){h, (uint32_t)(o + 8), len};
                set_add(&W.seen, seen_key(REC_SHADER, (uint64_t)px << 32 | h));
            } else if (type == REC_DECL) {
                uint32_t h;
                memcpy(&h, pl, 4);
                if (W.ndecl == cd) {
                    cd *= 2;
                    W.decls = realloc(W.decls, cd * sizeof(struct ent));
                }
                W.decls[W.ndecl++] = (struct ent){h, (uint32_t)(o + 8), len};
                set_add(&W.seen, seen_key(REC_DECL, h));
            } else {
                W.pipe_off[W.npipe++] = (uint32_t)(o + 8);
                set_add(&W.seen, seen_key(REC_PIPE, fnv64(pl + 12, len - 12, 0xcbf29ce484222325ull)));
            }
            o += 8 + len;
        }
    }

    /* arquivo para acrescentar: o prefixo válido (descarta a cauda cortada) ou um novo */
    W.f = fopen(path, "wb");
    if (W.f) {
        fwrite(&want, sizeof want, 1, W.f);
        if (keep && valid > sizeof(man_header)) fwrite(buf + sizeof(man_header), 1, valid - sizeof(man_header), W.f);
        fflush(W.f);
    } else {
        RE5_WARN(CH, "manifesto de shaders não pôde ser gravado em %s: sem pré-compilação no próximo arranque", path);
    }
    if (!keep || !W.npipe) {
        free(buf);
        free(W.pipe_off);
        free(W.shaders[0]);
        free(W.shaders[1]);
        free(W.decls);
        W.pipe_off = NULL;
        W.shaders[0] = W.shaders[1] = NULL;
        W.decls = NULL;
        RE5_INFO(CH, "pré-compilação: manifesto vazio (%s); este arranque aprende os pipelines do jogo", path);
        return;
    }
    W.buf = buf;
    W.total = W.npipe;
    int cpus = SDL_GetCPUCount();
    int nt = getenv("RE5VK2_PRECOMPILE_THREADS") ? atoi(getenv("RE5VK2_PRECOMPILE_THREADS")) : (W.mode == 2 ? cpus - 2 : 2);
    if (nt < 1) nt = 1;
    if (nt > 4) nt = 4;
    W.workers_left = (uint32_t)nt;
    RE5_INFO(CH, "pré-compilação: %u pipelines (%u VS, %u PS) do manifesto em %d thread(s)%s", W.npipe, W.nsh[0], W.nsh[1], nt,
             W.mode == 2 ? "; o primeiro Present espera (RE5VK2_PRECOMPILE=wait)" : "");
    W.started = 1;
    for (int i = 0; i < nt; i++)
        if (re5_host_thread_start(warm_worker, NULL, 512 * 1024) != 0) {
            RE5_WARN(CH, "thread de pré-compilação não criada");
            if (__atomic_sub_fetch(&W.workers_left, 1, __ATOMIC_ACQ_REL) == 0) W.started = 0;
        }
}

/* Modo wait: o primeiro Present espera a compilação acabar (com prazo). Sem o mutex do device na mão. */
void vk2_warm_wait_first_present(vk_device *d)
{
    (void)d;
    if (W.mode != 2 || !W.started) return;
    uint32_t waited_ms = 0, last_log = 0;
    while (__atomic_load_n(&W.workers_left, __ATOMIC_ACQUIRE) && waited_ms < W.timeout_s * 1000u) {
        re5_host_sleep_ms(20);
        waited_ms += 20;
        if (waited_ms - last_log >= 2000) {
            last_log = waited_ms;
            RE5_INFO(CH, "pré-compilação: %u/%u pipelines", __atomic_load_n(&W.done, __ATOMIC_RELAXED), W.total);
        }
    }
    if (__atomic_load_n(&W.workers_left, __ATOMIC_ACQUIRE))
        RE5_WARN(CH, "pré-compilação passou de %u s: o jogo segue e ela termina em segundo plano", W.timeout_s);
}
