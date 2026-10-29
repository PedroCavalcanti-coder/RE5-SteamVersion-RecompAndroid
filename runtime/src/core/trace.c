/*
 * trace.c - pontos de rastreio das funções do guest (config/tracepoints.txt).
 *
 * O run_lift.py envolve cada função listada em re5_trace_enter/re5_trace_exit.
 * Sem RE5_TRACE nada é impresso (um teste de flag por chamada). Com RE5_TRACE=1:
 *
 *   trace  [tid 0x1010]  > 007E2480 FUN_007e2480 ecx=0A1B2C3C [esp+4..]=00000001 ...
 *   trace  [tid 0x1010]  < 007E2480 eax=00000000
 *
 * RE5_TRACE_ARGS=<n> (padrão 4) é o número de argumentos da pilha mostrados.
 * RE5_TRACE_MAX=<n>: as primeiras n chamadas de cada função são impressas (padrão
 * 2000); depois só 1 em cada RE5_TRACE_SAMPLE (padrão 1000), para não afogar o log
 * de uma função que roda a cada quadro e ainda mostrar o que ela faz no jogo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/log.h"
#include "re5/runtime.h"
#include "re5/thunks.h"
#include "re5/guest_memory.h"
#include "re5/trace_packet.h"

/* Cobertura de funções: re5_cov[va - 0x401000] = 1 quando a função gerada de entrada `va` roda
 * (o run_lift.py emite o store, só num build com -DRE5_COVERAGE=ON). Com RE5_COV=1: F8 zera, F9
 * grava o que rodou desde o F8 em <RE5_COV_DIR ou work/cov>/cov_<n>.txt ("VA nome" por linha).
 * Comparar um arquivo "parado" com um "quebrando a caixa" mostra só o código daquela ação. */
#define RE5_COV_SIZE 0xB4A000u
unsigned char re5_cov[RE5_COV_SIZE];

void re5_cov_key(int fkey)
{
    static int enabled = -1, seq;
    if (enabled < 0) {
        const char *e = getenv("RE5_COV");
        enabled = e && *e && *e != '0';
#ifndef RE5_COVERAGE
        if (enabled)
            RE5_WARN("cov", "RE5_COV=1, mas o código gerado foi compilado sem cobertura: refaça o build com "
                            "-DRE5_COVERAGE=ON (o F9 vai gravar um arquivo vazio)");
#endif
    }
    if (!enabled) return;
    if (fkey == 8) {
        memset(re5_cov, 0, sizeof re5_cov);
        RE5_INFO("cov", "F8: cobertura zerada");
    } else if (fkey == 9) {
        const char *dir = getenv("RE5_COV_DIR");
        char path[512];
        snprintf(path, sizeof path, "%s/cov_%d.txt", dir && *dir ? dir : "work/cov", ++seq);
        FILE *f = fopen(path, "w");
        if (!f) {
            RE5_ERROR("cov", "não consegui gravar %s (crie a pasta)", path);
            return;
        }
        uint32_t n = 0;
        for (uint32_t i = 0; i < RE5_COV_SIZE; i++) {
            if (!re5_cov[i]) continue;
            uint32_t va = 0x401000u + i;
            const char *nm = re5_name_lookup(va);
            fprintf(f, "%08X %s\n", va, nm ? nm : "-");
            n++;
        }
        fclose(f);
        RE5_INFO("cov", "F9: %u funções executadas desde o F8 -> %s", n, path);
    }
}

/* ---- captura de uma chamada (F11 + RE5_CAPTURE=<VA de um tracepoint>) ----
 * Na entrada: work/dump/capture_<n>_antes.{bin,map,json}; na saída: capture_<n>_depois.*; mais
 * capture_<n>_thunks.tsv (VA, bytes de argumento, nome de cada thunk do host). O scripts/snap_diff.py
 * reexecuta o x86 original no Unicorn a partir do "antes" e compara com o "depois" do C gerado. */
/* RE5_CAPTURE=va[,va...] (até 8): cada F11 arma uma captura de cada endereço da lista; a próxima
 * chamada de cada um é gravada (uma de cada vez: chamadas aninhadas da lista esperam a próxima).
 * RE5_CAPTURE_N=<n> (padrão 1): grava as n próximas chamadas de cada um - p.ex. 2 para pegar os dois
 * personagens que passam pela mesma função de movimento no mesmo quadro. */
static uint32_t g_cap_list[8], g_cap_left[8], g_cap_n, g_cap_per, g_cap_va;
static volatile uint32_t g_cap_armed;   /* bit i: falta capturar g_cap_list[i] */
static volatile int g_cap_busy;
static uint32_t g_cap_tid, g_cap_seq;

void re5_capture_key(void)
{
    if (!g_cap_n) {
        const char *e = getenv("RE5_CAPTURE");
        for (const char *p = e; p && *p && g_cap_n < 8;) {
            char *end;
            uint32_t v = (uint32_t)strtoul(p, &end, 16);
            if (end == p) break;
            g_cap_list[g_cap_n++] = v;
            p = *end == ',' ? end + 1 : end;
        }
    }
    if (!g_cap_n) return;
    if (!g_cap_per) {
        const char *n = getenv("RE5_CAPTURE_N");
        g_cap_per = n && *n && strtoul(n, NULL, 0) > 0 ? (uint32_t)strtoul(n, NULL, 0) : 1u;
    }
    for (uint32_t i = 0; i < g_cap_n; i++) g_cap_left[i] = g_cap_per;
    g_cap_armed = (1u << g_cap_n) - 1u;
    RE5_INFO("capture", "F11: as próximas %u chamada(s) de cada um dos %u endereços de RE5_CAPTURE serão capturadas",
             g_cap_per, g_cap_n);
}

void re5_capture_at_present(uint32_t present)
{
    static int init;
    static uint32_t at[16], nat;
    if (!init) {
        init = 1;
        const char *e = getenv("RE5_CAPTURE_AT");
        for (const char *p = e; p && *p && nat < 16;) {
            char *end;
            uint32_t v = (uint32_t)strtoul(p, &end, 10);
            if (end == p) break;
            at[nat++] = v;
            p = *end == ',' ? end + 1 : end;
        }
    }
    for (uint32_t i = 0; i < nat; i++)
        if (at[i] == present) {
            RE5_INFO("capture", "RE5_CAPTURE_AT: Present %u", present);
            re5_capture_key();
        }
}

static void cap_write_regs(const CPU *c, const char *name)
{
    const char *dir = getenv("RE5_DUMP_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/%s.json", dir && *dir ? dir : "work/dump", name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    re5_thread *t = re5_thread_current();
    fprintf(f, "{\"tid\": %u, \"va\": %u,\n", t ? t->tid : 0, g_cap_va);
    fprintf(f, " \"eax\": %u, \"ecx\": %u, \"edx\": %u, \"ebx\": %u, \"esp\": %u, \"ebp\": %u, \"esi\": %u, \"edi\": %u,\n",
            c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    fprintf(f, " \"cf\": %u, \"zf\": %u, \"sf\": %u, \"of\": %u, \"pf\": %u, \"af\": %u,\n", c->cf, c->zf, c->sf, c->of,
            c->pf, c->af);
    fprintf(f, " \"fpu_top\": %d, \"fpu_sw\": %u, \"fpu_cw\": %u, \"mxcsr\": %u, \"fs_base\": %u,\n", c->fpu_top,
            c->fpu_sw, c->fpu_cw, c->mxcsr, c->fs_base);
    fprintf(f, " \"st\": [");
    for (int i = 0; i < 8; i++) {
        uint64_t b;
        memcpy(&b, &c->st[i], 8);
        fprintf(f, "%s\"%016llX\"", i ? ", " : "", (unsigned long long)b);
    }
    fprintf(f, "],\n \"xmm\": [");
    for (int i = 0; i < 8; i++) {
        const uint8_t *x = (const uint8_t *)&c->xmm[i];
        fprintf(f, "%s\"", i ? ", " : "");
        for (int k = 0; k < 16; k++) fprintf(f, "%02X", x[k]);
        fprintf(f, "\"");
    }
    fprintf(f, "]}\n");
    fclose(f);
}

static void cap_write_thunks(const char *name)
{
    const char *dir = getenv("RE5_DUMP_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/%s.tsv", dir && *dir ? dir : "work/dump", name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (uint32_t i = 0; i < re5_thunk_count(); i++) {
        uint32_t va = RE5_THUNK_BASE + i * RE5_THUNK_STRIDE;
        const re5_thunk *t = re5_thunk_get(va);
        if (t) fprintf(f, "%08X\t%u\t%s\n", va, t->arg_bytes, t->name ? t->name : "?");
    }
    fclose(f);
}

static void cap_enter(CPU *c, uint32_t va)
{
    if (!g_cap_armed || g_cap_busy) return;
    uint32_t i = 0;
    while (i < g_cap_n && !(g_cap_list[i] == va && (g_cap_armed >> i) & 1u)) i++;
    if (i == g_cap_n) return;
    /* Duas threads podem chegar juntas (inimigos atualizados em paralelo): só uma captura. */
    if (!__atomic_compare_exchange_n(&g_cap_busy, &(int){0}, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    re5_thread *t = re5_thread_current();
    if (--g_cap_left[i] == 0) g_cap_armed &= ~(1u << i);
    g_cap_va = va;
    g_cap_tid = t ? t->tid : 0;
    char name[64];
    snprintf(name, sizeof name, "capture_%u_antes", ++g_cap_seq);
    cap_write_regs(c, name);
    re5_dump_named(name);
    snprintf(name, sizeof name, "capture_%u_thunks", g_cap_seq);
    cap_write_thunks(name);
}

static void cap_exit(CPU *c, uint32_t va)
{
    if (!g_cap_busy || va != g_cap_va) return;
    re5_thread *t = re5_thread_current();
    if ((t ? t->tid : 0) != g_cap_tid) return;
    char name[64];
    snprintf(name, sizeof name, "capture_%u_depois", g_cap_seq);
    cap_write_regs(c, name);
    re5_dump_named(name);
    g_cap_busy = 0;
    RE5_INFO("capture", "chamada de %08X capturada: work/dump/capture_%u_{antes,depois}", va, g_cap_seq);
}

typedef struct {
    uint32_t va;
    uint32_t count;
} trace_slot;

#define TRACE_SLOTS 512
static trace_slot g_slots[TRACE_SLOTS];
static volatile int g_enabled = -1;
static uint32_t g_nargs = 4, g_max = 2000, g_sample = 1000;

static int trace_on(void)
{
    if (g_enabled < 0) {
        const char *e = getenv("RE5_TRACE");
        const char *a = getenv("RE5_TRACE_ARGS");
        const char *m = getenv("RE5_TRACE_MAX");
        const char *sm = getenv("RE5_TRACE_SAMPLE");
        if (a && *a) g_nargs = (uint32_t)strtoul(a, NULL, 0) > 8 ? 8 : (uint32_t)strtoul(a, NULL, 0);
        if (m && *m) g_max = (uint32_t)strtoul(m, NULL, 0);
        if (sm && *sm && strtoul(sm, NULL, 0) > 0) g_sample = (uint32_t)strtoul(sm, NULL, 0);
        g_enabled = e && *e && *e != '0';
    }
    return g_enabled;
}

/* Contador por função (aproximado entre threads: é só um limite de log). */
static uint32_t bump(uint32_t va)
{
    uint32_t i = (va * 2654435761u) % TRACE_SLOTS;
    for (uint32_t n = 0; n < TRACE_SLOTS; n++, i = (i + 1) % TRACE_SLOTS) {
        if (g_slots[i].va == va || g_slots[i].va == 0) {
            g_slots[i].va = va;
            return ++g_slots[i].count;
        }
    }
    return 0;
}

void re5_trace_enter(CPU *c, uint32_t va)
{
    re5_det_on_trace(c, va, 1);
    cap_enter(c, va);
    if (!trace_on()) return;
    uint32_t n = bump(va);
    if (n > g_max) {
        if (n == g_max + 1)
            RE5_INFO("trace", "%08X: passadas %u chamadas, só 1 em cada %u é impressa", va, g_max, g_sample);
        if (n % g_sample) return;
    }
    char args[96];
    int len = 0;
    for (uint32_t i = 0; i < g_nargs && len < (int)sizeof args - 10; i++) {
        uint32_t a = c->esp + 4u * (i + 1);
        len += snprintf(args + len, sizeof args - (size_t)len, " %08X",
                        re5_mem_is_mapped(a, 4) ? rd32(a) : 0xDEADDEADu);
    }
    /* RE5_TRACE_PEEK=va,va,...: valores dessas posições do guest na entrada (p.ex. campos de um singleton) */
    static uint32_t peek[8], npeek;
    static int peek_init;
    if (!peek_init) {
        const char *e = getenv("RE5_TRACE_PEEK");
        for (const char *p = e; p && *p && npeek < 8;) {
            char *end;
            uint32_t v = (uint32_t)strtoul(p, &end, 16);
            if (end == p) break;
            peek[npeek++] = v;
            p = *end == ',' ? end + 1 : end;
        }
        peek_init = 1;
    }
    char pk[128];
    int pl = 0;
    for (uint32_t i = 0; i < npeek && pl < (int)sizeof pk - 14; i++)
        pl += snprintf(pk + pl, sizeof pk - (size_t)pl, " [%X]=%08X", peek[i],
                       re5_mem_is_mapped(peek[i], 4) ? rd32(peek[i]) : 0xDEADDEADu);
    pk[pl] = 0;
    args[len] = 0;
    re5_thread *t = re5_thread_current();
    const char *nm = re5_name_lookup(va);
    uint32_t ret = re5_mem_is_mapped(c->esp, 4) ? rd32(c->esp) : 0;
    RE5_INFO("trace", "[tid 0x%X] > %08X %s ecx=%08X ret=%08X args:%s%s", t ? t->tid : 0, va, nm ? nm : "", c->ecx,
             ret, args, pk);
}

void re5_trace_exit(CPU *c, uint32_t va)
{
    re5_det_on_trace(c, va, 2);
    cap_exit(c, va);
    if (!trace_on()) return;
    uint32_t i = (va * 2654435761u) % TRACE_SLOTS;
    for (uint32_t k = 0; k < TRACE_SLOTS && g_slots[i].va != va; k++) i = (i + 1) % TRACE_SLOTS;
    if (g_slots[i].va != va || (g_slots[i].count > g_max && g_slots[i].count % g_sample)) return;
    re5_thread *t = re5_thread_current();
    /* *eax: o hkBool que o Havok devolve por ponteiro oculto (eax = esse ponteiro). */
    if (c->eax >= 0x10000u && re5_mem_is_mapped(c->eax, 1))
        RE5_INFO("trace", "[tid 0x%X] < %08X eax=%08X *eax=%02X", t ? t->tid : 0, va, c->eax, rd8(c->eax));
    else
        RE5_INFO("trace", "[tid 0x%X] < %08X eax=%08X", t ? t->tid : 0, va, c->eax);
}


/* =========================================================================
 * Deterministic Replay & Trace Diff (TAS / Lockstep)
 * ========================================================================= */
static int g_det_enabled = -1;
static volatile int g_det_tas_active = 0;
static uint32_t g_tas_frame = 0;
static uint32_t g_tas_start = 120;
static uint32_t g_tas_end = 600;
static int16_t  g_tas_stick_x = 0;
static int16_t  g_tas_stick_y = 32767;
static int      g_tas_key = 7; /* F7 padrão */
static uint64_t g_det_qpc = 100000000ull;
static uint32_t g_det_tick = 10000u;
static FILE    *g_det_file = NULL;
static char     g_trace_path[512] = "work/trace_recomp.bin";
static char     g_snapshot_path[512] = "snapshot_s115.json";
static int      g_snapshot_apply = 1;

void re5_det_init(void)
{
    if (g_det_enabled >= 0) return;
    const char *e = getenv("RE5_DETERMINISTIC");
    if (!e) e = getenv("RE5_TRACE_DIFF");
    g_det_enabled = e && *e && *e != '0';
    if (!g_det_enabled) return;

    const char *p = getenv("RE5_TRACE_FILE");
    if (p && *p) snprintf(g_trace_path, sizeof g_trace_path, "%s", p);

    const char *sp = getenv("RE5_SNAPSHOT_FILE");
    if (sp && *sp) snprintf(g_snapshot_path, sizeof g_snapshot_path, "%s", sp);

    const char *k = getenv("RE5_TAS_KEY");
    if (k && *k) g_tas_key = (int)strtol(k, NULL, 0);

    const char *s = getenv("RE5_TAS_START");
    if (s && *s) g_tas_start = (uint32_t)strtoul(s, NULL, 0);

    const char *end = getenv("RE5_TAS_END");
    if (end && *end) g_tas_end = (uint32_t)strtoul(end, NULL, 0);

    const char *sy = getenv("RE5_TAS_STICK_Y");
    if (sy && *sy) g_tas_stick_y = (int16_t)strtol(sy, NULL, 0);

    const char *sx = getenv("RE5_TAS_STICK_X");
    if (sx && *sx) g_tas_stick_x = (int16_t)strtol(sx, NULL, 0);

    RE5_INFO("det", "Deterministic Replay ativo: 60.0 FPS travado. Tecla F%d inicia TAS (%u..%u quadros) -> %s",
             g_tas_key, g_tas_start, g_tas_end, g_trace_path);
}

int re5_det_is_enabled(void)
{
    if (g_det_enabled < 0) re5_det_init();
    return g_det_enabled && g_det_tas_active;
}

uint64_t re5_det_qpc(void)
{
    if (g_det_enabled < 0) re5_det_init();
    return g_det_qpc;
}

uint32_t re5_det_tick(void)
{
    if (g_det_enabled < 0) re5_det_init();
    return g_det_tick;
}

void re5_det_on_present(void)
{
    if (g_det_enabled < 0) re5_det_init();
    if (!g_det_enabled) return;

    if (g_det_tas_active) {
        g_det_qpc += (uint64_t)(10000000.0 / 60.0);
        g_det_tick += (uint32_t)(1000.0 / 60.0);
        g_tas_frame++;
        if (g_tas_frame > g_tas_end) {
            g_det_tas_active = 0;
            if (g_det_file) {
                fclose(g_det_file);
                g_det_file = NULL;
            }
            RE5_INFO("det", "TAS & Gravação concluídos com sucesso! %u quadros em %s", g_tas_end, g_trace_path);
        }
    }
}

int re5_det_get_tas_input(uint32_t user, int16_t *stickX, int16_t *stickY)
{
    if (g_det_enabled < 0) re5_det_init();
    if (!g_det_enabled || !g_det_tas_active || user != 0) return 0;

    if (g_tas_frame >= g_tas_start && g_tas_frame <= g_tas_end) {
        if (stickX) *stickX = g_tas_stick_x;
        if (stickY) *stickY = g_tas_stick_y;
    } else {
        if (stickX) *stickX = 0;
        if (stickY) *stickY = 0;
    }
    return 1;
}

static void re5_det_trigger_tas(void)
{
    if (g_det_enabled < 0) re5_det_init();
    if (!g_det_enabled) return;

    if (!g_det_tas_active) {
        g_det_tas_active = 1;
        g_tas_frame = 0;
        g_snapshot_apply = 1;
        if (g_det_file) fclose(g_det_file);
        g_det_file = fopen(g_trace_path, "wb");
        if (!g_det_file) {
            RE5_ERROR("det", "Não foi possível abrir %s para gravação!", g_trace_path);
            g_det_tas_active = 0;
            return;
        }
        RE5_INFO("det", "Iniciando gravação do TAS (%u..%u quadros) em %s...", g_tas_start, g_tas_end, g_trace_path);
    } else {
        g_det_tas_active = 0;
        if (g_det_file) {
            fclose(g_det_file);
            g_det_file = NULL;
        }
        RE5_INFO("det", "Gravação do TAS interrompida no quadro %u.", g_tas_frame);
    }
}

void re5_det_key(int fkey)
{
    if (fkey == g_tas_key || (g_tas_key == 7 && fkey == 7) || (g_tas_key == 10 && fkey == 10)) {
        re5_det_trigger_tas();
    }
}

static void apply_snapshot(uint32_t player)
{
    FILE *f = fopen(g_snapshot_path, "r");
    if (!f) {
        char alt[576];
        snprintf(alt, sizeof alt, "work/%s", g_snapshot_path);
        f = fopen(alt, "r");
    }
    if (!f) return;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    float px, py, pz, rx, ry, rz, rw;
    char *ppos = strstr(buf, "\"pos\":");
    char *prot = strstr(buf, "\"rot\":");
    if (ppos && sscanf(ppos, "\"pos\": [%f, %f, %f]", &px, &py, &pz) == 3) {
        if (re5_mem_is_mapped(player + 0x30, 12)) {
            wrss(player + 0x30, px);
            wrss(player + 0x34, py);
            wrss(player + 0x38, pz);
            if (re5_mem_is_mapped(player + 0x1170, 12)) {
                wrss(player + 0x1170, px);
                wrss(player + 0x1174, py);
                wrss(player + 0x1178, pz);
            }
            RE5_INFO("det", "Snapshot transplant: Chris teleportado para (%.3f, %.3f, %.3f)", px, py, pz);
        }
    }
    if (prot && sscanf(prot, "\"rot\": [%f, %f, %f, %f]", &rx, &ry, &rz, &rw) == 4) {
        if (re5_mem_is_mapped(player + 0x40, 16)) {
            wrss(player + 0x40, rx);
            wrss(player + 0x44, ry);
            wrss(player + 0x48, rz);
            wrss(player + 0x4C, rw);
        }
    }
}

void re5_det_on_trace(CPU *c, uint32_t va, uint32_t hook_site)
{
    if (va != 0x00B72FA0u || !g_det_tas_active || !g_det_file) return;

    static uint32_t s_last_frame = 0xFFFFFFFFu;
    static uint32_t s_char_idx = 0;
    if (g_tas_frame != s_last_frame) {
        s_last_frame = g_tas_frame;
        s_char_idx = 0;
    }

    uint32_t charId = s_char_idx;
    if (hook_site == 2) {
        s_char_idx++;
    }

    uint32_t player = c->ecx;
    if (!re5_mem_is_mapped(player, 0x100)) return;

    if (hook_site == 1 && charId == 0 && g_snapshot_apply) {
        apply_snapshot(player);
        g_snapshot_apply = 0;
    }

    FrameTracePacket pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.magic = RE5_TRACE_MAGIC;
    pkt.frameIndex = g_tas_frame;
    pkt.hookSite = hook_site;
    pkt.characterId = charId;

    if (re5_mem_is_mapped(player + 0x10, 4)) pkt.deltaTime = rdss(player + 0x10);

    int16_t curX = 0, curY = 0;
    if (g_tas_frame >= g_tas_start && g_tas_frame <= g_tas_end) {
        curX = g_tas_stick_x;
        curY = g_tas_stick_y;
    }
    pkt.inputStickX = (float)curX / 32767.0f;
    pkt.inputStickY = (float)curY / 32767.0f;

    if (re5_mem_is_mapped(player + 0x30, 12)) {
        pkt.pos[0] = rdss(player + 0x30);
        pkt.pos[1] = rdss(player + 0x34);
        pkt.pos[2] = rdss(player + 0x38);
    }
    if (re5_mem_is_mapped(player + 0x1170, 12)) {
        pkt.oldPos[0] = rdss(player + 0x1170);
        pkt.oldPos[1] = rdss(player + 0x1174);
        pkt.oldPos[2] = rdss(player + 0x1178);
    }
    if (re5_mem_is_mapped(player + 0x1400, 12)) {
        pkt.moveVec[0] = rdss(player + 0x1400);
        pkt.moveVec[1] = rdss(player + 0x1404);
        pkt.moveVec[2] = rdss(player + 0x1408);
    }
    if (re5_mem_is_mapped(player + 0x2D7C, 4)) {
        pkt.collisionFlags = rd32(player + 0x2D7C);
    }
    pkt.filterMask = 0x3FFFFF00u;
    pkt.stepHeight = 35.0f;
    pkt.slopeCos = 0.70710678f;
    pkt.wallCos = 0.57357644f;

    if (re5_mem_is_mapped(player, 0x2000)) {
        pkt.playerStructCRC32 = re5_trace_crc32(re5_g2h(player), 0x2000);
    }

    fwrite(&pkt, sizeof pkt, 1, g_det_file);
    fflush(g_det_file);
}
