/*
 * test_vk_dxso.c - tradutor SM3 -> SPIR-V do backend re5vk (runtime/src/gfx/vk/vk_dxso.c).
 *
 * Sem argumentos: shaders vs_3_0/ps_3_0 montados à mão, cobrindo as instruções que o RE5 usa;
 * com RE5_SPIRV_VAL=<spirv-val> (o do NDK: shader-tools/<host>/spirv-val), cada SPIR-V gerado
 * passa pelo validador da Khronos.
 *
 * Com uma pasta: traduz cada <vs|ps>_*.bin (despejo do RE5_D3D9_SHADER_DUMP), nas variantes
 * sem e com profundidade/alpha test, e valida como acima. Imprime as falhas; sai 1 se houver.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "../../runtime/src/gfx/vk/vk_dxso.h"

/* ---- montagem de bytecode ---- */

#define VS3 0xFFFE0300u
#define PS3 0xFFFF0300u
#define END 0x0000FFFFu
#define INS(op, len) ((uint32_t)(op) | ((uint32_t)(len) << 24))
#define INSC(op, len, ctl) (INS(op, len) | ((uint32_t)(ctl) << 16))
#define RT(t) ((((uint32_t)(t) & 7u) << 28) | ((((uint32_t)(t) >> 3) & 3u) << 11))
#define DST(t, n, mask) (0x80000000u | RT(t) | ((uint32_t)(mask) << 16) | (uint32_t)(n))
#define DSTSAT(t, n, mask) (DST(t, n, mask) | (1u << 20))
#define SRC(t, n) (0x80000000u | RT(t) | (0xE4u << 16) | (uint32_t)(n))
#define SRCSW(t, n, sw) (0x80000000u | RT(t) | ((uint32_t)(sw) << 16) | (uint32_t)(n))
#define SRCMOD(t, n, mod) (SRC(t, n) | ((uint32_t)(mod) << 24))
#define DCL(usage, idx) (0x80000000u | (uint32_t)(usage) | ((uint32_t)(idx) << 16))
#define DCLS(tt) (0x80000000u | ((uint32_t)(tt) << 27))
#define XXXX 0x00u
#define WWWW 0xFFu

enum { TEMP = 0, INPUT = 1, CONST = 2, ADDR = 3, OUTPUT = 6, CONSTINT = 7, COLOROUT = 8, SAMPLER = 10,
       CONSTBOOL = 14, LOOP = 15, PRED = 19 };

static const char *g_val;   /* spirv-val */
static int g_fail;

static int validate(const re5vk_shader *s, const char *name)
{
    if (s->nwords < 5 || s->spv[0] != 0x07230203u) {
        printf("%s: SPIR-V sem cabeçalho\n", name);
        return -1;
    }
    if (!g_val) return 0;
    char path[512], cmd[1200];
    const char *tmp = getenv("TEMP") ? getenv("TEMP") : "/tmp";
    snprintf(path, sizeof path, "%s/re5vk_dxso_test.spv", tmp);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(s->spv, 4, s->nwords, f);
    fclose(f);
    snprintf(cmd, sizeof cmd, "\"\"%s\" --target-env vulkan1.1 \"%s\"\"", g_val, path);
#if !defined(_WIN32)
    snprintf(cmd, sizeof cmd, "'%s' --target-env vulkan1.1 '%s'", g_val, path);
#endif
    int rc = system(cmd);
    if (rc != 0) printf("%s: spirv-val recusou (%d); SPIR-V em %s\n", name, rc, path);
    return rc == 0 ? 0 : -1;
}

static void check_shader(const char *name, const uint32_t *code, size_t n, const re5vk_dxso_variant *v)
{
    re5vk_shader s;
    char err[256];
    if (re5vk_dxso_compile(code, n, v, &s, err, sizeof err) != 0) {
        printf("%s: %s\n", name, err);
        g_fail++;
        return;
    }
    if (validate(&s, name) != 0) g_fail++;
    free(s.spv);
}

/* ---- shaders de teste ---- */

static const uint32_t vs_basic[] = {
    VS3,
    INS(31, 2), DCL(0, 0), DST(INPUT, 0, 0xF),          /* dcl_position v0 */
    INS(31, 2), DCL(5, 0), DST(INPUT, 1, 0xF),          /* dcl_texcoord v1 */
    INS(31, 2), DCL(10, 0), DST(INPUT, 2, 0xF),         /* dcl_color v2 */
    INS(31, 2), DCL(0, 0), DST(OUTPUT, 0, 0xF),         /* dcl_position o0 */
    INS(31, 2), DCL(5, 0), DST(OUTPUT, 1, 0xF),         /* dcl_texcoord o1 */
    INS(31, 2), DCL(10, 0), DST(OUTPUT, 2, 0xF),        /* dcl_color o2 */
    INS(31, 2), DCL(11, 0), DST(OUTPUT, 3, 0x1),        /* dcl_fog o3.x */
    INS(81, 5), DST(CONST, 200, 0xF), 0x3F000000u, 0x3F800000u, 0x40000000u, 0u,   /* def c200 */
    INS(20, 2), DST(OUTPUT, 0, 0xF), SRC(INPUT, 0), SRC(CONST, 0),          /* m4x4 o0, v0, c0 */
    INS(4, 4), DST(TEMP, 0, 0x3), SRC(INPUT, 1), SRC(CONST, 200), SRC(CONST, 4),   /* mad r0.xy */
    INS(46, 2), DST(ADDR, 0, 0x1), SRCSW(INPUT, 2, WWWW),                   /* mova a0.x, v2.w */
    INS(1, 3), DST(TEMP, 1, 0xF), SRC(CONST, 10) | (1u << 13), SRCSW(ADDR, 0, XXXX),   /* mov r1, c[a0.x+10] */
    INS(9, 3), DST(TEMP, 0, 0x4), SRC(TEMP, 1), SRC(CONST, 5),             /* dp4 r0.z */
    INS(7, 2), DST(TEMP, 0, 0x8), SRCSW(TEMP, 0, 0xAA),                    /* rsq r0.w, r0.z */
    INS(1, 2), DST(OUTPUT, 1, 0xF), SRC(TEMP, 0),
    INS(1, 2), DSTSAT(OUTPUT, 2, 0xF), SRC(INPUT, 2),
    INS(1, 2), DST(OUTPUT, 3, 0x1), SRCMOD(INPUT, 0, 1),                   /* mov o3.x, -v0 */
    END,
};

static const uint32_t ps_basic[] = {
    PS3,
    INS(31, 2), DCL(5, 0), DST(INPUT, 0, 0x3),          /* dcl_texcoord v0.xy */
    INS(31, 2), DCL(10, 0), DST(INPUT, 1, 0xF),         /* dcl_color v1 */
    INS(31, 2), DCLS(2), DST(SAMPLER, 0, 0xF),          /* dcl_2d s0 */
    INS(31, 2), DCLS(3), DST(SAMPLER, 1, 0xF),          /* dcl_cube s1 */
    INS(31, 2), DCLS(4), DST(SAMPLER, 2, 0xF),          /* dcl_volume s2 */
    INS(31, 2), DCLS(2), DST(SAMPLER, 3, 0xF),          /* dcl_2d s3 (profundidade na variante) */
    INS(66, 3), DST(TEMP, 0, 0xF), SRC(INPUT, 0), SRC(SAMPLER, 0),          /* texld r0, v0, s0 */
    INSC(66, 3, 1), DST(TEMP, 1, 0xF), SRC(INPUT, 1), SRC(SAMPLER, 0),      /* texldp */
    INSC(66, 3, 2), DST(TEMP, 2, 0xF), SRC(INPUT, 1), SRC(SAMPLER, 1),      /* texldb (cubo) */
    INS(95, 3), DST(TEMP, 3, 0xF), SRC(INPUT, 1), SRC(SAMPLER, 2),          /* texldl (volume) */
    INS(66, 3), DST(TEMP, 4, 0xF), SRC(INPUT, 1), SRC(SAMPLER, 3),          /* texld s3 */
    INS(88, 4), DST(TEMP, 0, 0xF), SRC(TEMP, 0), SRC(TEMP, 1), SRC(TEMP, 2),   /* cmp */
    INS(18, 4), DST(TEMP, 0, 0xF), SRC(CONST, 0), SRC(TEMP, 0), SRC(TEMP, 3),  /* lrp */
    INS(90, 4), DST(TEMP, 5, 0x1), SRC(TEMP, 0), SRC(TEMP, 4), SRC(CONST, 1),  /* dp2add */
    INS(36, 2), DST(TEMP, 6, 0x7), SRC(TEMP, 0),                               /* nrm */
    INS(32, 3), DST(TEMP, 5, 0x2), SRCSW(TEMP, 5, XXXX), SRCSW(CONST, 2, XXXX),   /* pow */
    INS(37, 2), DST(TEMP, 7, 0x3), SRCSW(TEMP, 5, XXXX),                       /* sincos */
    INS(91, 2), DST(TEMP, 8, 0xF), SRC(INPUT, 0),                              /* dsx */
    INS(65, 1), DST(TEMP, 0, 0xF),                                             /* texkill r0 */
    INS(5, 3), DST(COLOROUT, 0, 0xF), SRC(TEMP, 0), SRC(INPUT, 1),
    END,
};

static const uint32_t ps_flow[] = {
    PS3,
    INS(31, 2), DCL(5, 0), DST(INPUT, 0, 0xF),
    INS(48, 5), DST(CONSTINT, 0, 0xF), 4, 0, 1, 0,                         /* defi i0, 4, 0, 1, 0 */
    INS(47, 2), DST(CONSTBOOL, 1, 0xF), 1,                                 /* defb b1, true */
    INS(1, 2), DST(TEMP, 0, 0xF), SRC(INPUT, 0),
    INS(27, 2), SRC(LOOP, 0), SRC(CONSTINT, 0),                            /* loop aL, i0 */
    INS(2, 4), DST(TEMP, 0, 0xF), SRC(TEMP, 0), SRC(CONST, 0) | (1u << 13), SRCSW(LOOP, 0, XXXX),   /* c[aL] */
    INSC(45, 2, 1), SRCSW(TEMP, 0, XXXX), SRCSW(CONST, 9, XXXX),           /* break_gt */
    INS(29, 0),                                                            /* endloop */
    INS(38, 1), SRC(CONSTINT, 1),                                          /* rep i1 (do UBO) */
    INS(40, 1), SRC(CONSTBOOL, 0),                                         /* if b0 (do UBO) */
    INS(5, 3), DST(TEMP, 0, 0xF), SRC(TEMP, 0), SRC(CONST, 2),
    INS(44, 0),                                                            /* break */
    INS(5, 3), DST(TEMP, 0, 0xF), SRC(TEMP, 0), SRC(CONST, 2),             /* código morto depois do break */
    INS(42, 0),                                                            /* else */
    INS(2, 3), DST(TEMP, 0, 0xF), SRC(TEMP, 0), SRC(CONST, 3),
    INS(43, 0),                                                            /* endif */
    INS(39, 0),                                                            /* endrep */
    INSC(41, 2, 4), SRCSW(TEMP, 0, XXXX), SRCSW(CONST, 4, XXXX),           /* ifc_lt */
    INS(40, 1), SRC(CONSTBOOL, 1),                                         /* if b1 (def) */
    INS(19, 2), DST(TEMP, 0, 0xF), SRC(TEMP, 0),                           /* frc */
    INS(43, 0),
    INS(43, 0),
    INSC(94, 3, 3), DST(PRED, 0, 0xF), SRC(TEMP, 0), SRC(CONST, 5),        /* setp_ge p0 */
    INS(1, 3) | (1u << 28), DST(TEMP, 0, 0x3), SRC(PRED, 0), SRC(CONST, 6),   /* (p0) mov r0.xy */
    INS(40, 1), SRCSW(PRED, 0, 0x55),                                      /* if p0.y */
    INS(1, 2), DST(TEMP, 0, 0x8), SRC(CONST, 7),
    INS(43, 0),
    INS(1, 2), DST(COLOROUT, 0, 0xF), SRC(TEMP, 0),
    INS(1, 2), DST(9, 0, 0x1), SRCSW(TEMP, 0, XXXX),                       /* oDepth */
    END,
};

static void run_builtin(void)
{
    re5vk_dxso_variant plain = {0, 0}, depth = {1u << 3, 5 /* GREATER */};
    check_shader("vs_basic", vs_basic, sizeof vs_basic / 4, &plain);
    check_shader("ps_basic", ps_basic, sizeof ps_basic / 4, &plain);
    check_shader("ps_basic (profundidade + alpha test)", ps_basic, sizeof ps_basic / 4, &depth);
    check_shader("ps_flow", ps_flow, sizeof ps_flow / 4, &plain);

    re5vk_shader s;
    char err[256];
    CHECK(re5vk_dxso_compile(vs_basic, sizeof vs_basic / 4, &plain, &s, err, sizeof err) == 0);
    CHECK_EQ_U32(s.inputs, 0x7);
    CHECK_EQ_U32(s.in_usage[1], 5);
    CHECK_EQ_U32(s.pixel, 0);
    free(s.spv);
    CHECK(re5vk_dxso_compile(ps_basic, sizeof ps_basic / 4, &plain, &s, err, sizeof err) == 0);
    CHECK_EQ_U32(s.samplers, 0xF);
    CHECK_EQ_U32(s.sampler_type[1], RE5VK_TEX_CUBE);
    free(s.spv);
    CHECK(re5vk_dxso_compile(ps_flow, sizeof ps_flow / 4, &plain, &s, err, sizeof err) == 0);
    CHECK_EQ_U32(s.writes_depth, 1);
    free(s.spv);

    /* Falhar alto: instrução sem tradução (call) diz qual e onde. */
    const uint32_t bad[] = {VS3, INS(25, 1), 0xA0000000u, END};
    CHECK(re5vk_dxso_compile(bad, 4, &plain, &s, err, sizeof err) != 0);
    CHECK(strstr(err, "opcode 25") != NULL);
    const uint32_t sm2[] = {0xFFFF0200u, END};
    CHECK(re5vk_dxso_compile(sm2, 2, &plain, &s, err, sizeof err) != 0);
}

static int run_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        printf("pasta %s não abre\n", dir);
        return 1;
    }
    int n = 0;
    for (struct dirent *e; (e = readdir(d));) {
        size_t l = strlen(e->d_name);
        if (l < 8 || strcmp(e->d_name + l - 4, ".bin")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t *code = malloc(1u << 20);
        size_t nw = fread(code, 4, (1u << 20) / 4, f);
        fclose(f);
        re5vk_dxso_variant v0 = {0, 0}, v1 = {0xFFFF, 4};
        check_shader(e->d_name, code, nw, &v0);
        if (e->d_name[0] == 'p') check_shader(e->d_name, code, nw, &v1);
        free(code);
        n++;
    }
    closedir(d);
    printf("%d shaders, %d falhas\n", n, g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    g_val = getenv("RE5_SPIRV_VAL");
    if (g_val && !*g_val) g_val = NULL;
    if (argc > 1) return run_dir(argv[1]);
    run_builtin();
    CHECK_EQ_U32(g_fail, 0);
    CHECK_DONE();
}
