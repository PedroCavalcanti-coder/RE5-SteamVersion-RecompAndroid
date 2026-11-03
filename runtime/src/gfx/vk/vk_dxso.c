/*
 * vk_dxso.c - SM3 (vs_3_0 / ps_3_0) -> SPIR-V 1.0 para o backend re5vk (ver vk_dxso.h).
 *
 * Tradução direta, sem SSA: cada registrador do D3D9 é uma variável vec4 da função (temporários,
 * saídas) ou do módulo (entradas, constantes, samplers); cada instrução carrega as fontes com
 * swizzle e modificador, calcula, e grava com a máscara (e o predicado). As saídas são copiadas
 * para as variáveis de saída do Vulkan no fim do main. Fluxo de controle do D3D9 (if/else/
 * loop/rep/break) vira as construções estruturadas do SPIR-V.
 *
 * As regras numéricas do D3D9 seguem as da DXVK (src/dxso): rsq/log/pow usam |x|, mova arredonda,
 * vPos tem o centro do pixel nos inteiros. Instrução, registrador ou modificador fora do que o
 * RE5 usa falha alto com o deslocamento no bytecode (D8).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_dxso.h"

/* ---- buffer de palavras ---- */

typedef struct {
    uint32_t *w;
    size_t n, cap;
} wbuf;

static void put(wbuf *b, uint32_t v)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->w = realloc(b->w, b->cap * sizeof *b->w);
        if (!b->w) abort();
    }
    b->w[b->n++] = v;
}

static void op_emit(wbuf *b, uint32_t op, const uint32_t *a, size_t n)
{
    put(b, (uint32_t)((n + 1) << 16) | op);
    for (size_t i = 0; i < n; i++) put(b, a[i]);
}
#define OP(b, op, ...) op_emit((b), (op), (const uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / 4)
#define OP0(b, op) op_emit((b), (op), NULL, 0)

static void put_str(wbuf *b, const char *s)
{
    size_t n = strlen(s) + 1;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = 0;
        for (size_t k = 0; k < 4 && i + k < n; k++) v |= (uint32_t)(uint8_t)s[i + k] << (8 * k);
        put(b, v);
    }
}

/* Opcodes do SPIR-V usados aqui. */
enum {
    SOpExtInstImport = 11, SOpExtInst = 12, SOpMemoryModel = 14, SOpEntryPoint = 15, SOpExecutionMode = 16,
    SOpCapability = 17, SOpTypeVoid = 19, SOpTypeBool = 20, SOpTypeInt = 21, SOpTypeFloat = 22,
    SOpTypeVector = 23, SOpTypeImage = 25, SOpTypeSampledImage = 27, SOpTypeArray = 28, SOpTypeStruct = 30,
    SOpTypePointer = 32, SOpTypeFunction = 33, SOpConstantTrue = 41, SOpConstantFalse = 42, SOpConstant = 43,
    SOpConstantComposite = 44, SOpSpecConstant = 50, SOpFunction = 54, SOpFunctionEnd = 56, SOpVariable = 59,
    SOpLoad = 61, SOpStore = 62, SOpAccessChain = 65, SOpDecorate = 71, SOpMemberDecorate = 72,
    SOpVectorShuffle = 79, SOpCompositeConstruct = 80, SOpCompositeExtract = 81,
    SOpImageSampleImplicitLod = 87, SOpImageSampleExplicitLod = 88, SOpImageSampleDrefImplicitLod = 89,
    SOpImageSampleDrefExplicitLod = 90, SOpImageSampleProjImplicitLod = 91, SOpImageSampleProjDrefImplicitLod = 93,
    SOpConvertFToS = 110, SOpConvertSToF = 111, SOpFNegate = 127, SOpIAdd = 128, SOpFAdd = 129, SOpFSub = 131,
    SOpFMul = 133, SOpFDiv = 136, SOpVectorTimesScalar = 142, SOpDot = 148, SOpAny = 154, SOpLogicalOr = 166,
    SOpLogicalAnd = 167, SOpLogicalNot = 168, SOpSelect = 169, SOpINotEqual = 171, SOpSLessThan = 177,
    SOpFOrdEqual = 180, SOpFOrdNotEqual = 182, SOpFOrdLessThan = 184, SOpFOrdGreaterThan = 186,
    SOpFOrdLessThanEqual = 188, SOpFOrdGreaterThanEqual = 190, SOpShiftRightLogical = 194, SOpBitwiseAnd = 199,
    SOpDPdx = 207, SOpDPdy = 208, SOpLoopMerge = 246, SOpSelectionMerge = 247, SOpLabel = 248, SOpBranch = 249,
    SOpBranchConditional = 250, SOpKill = 252, SOpReturn = 253,
};
/* GLSL.std.450 */
enum { GFloor = 8, GFract = 10, GSin = 13, GCos = 14, GPow = 26, GExp2 = 29, GLog2 = 30, GInverseSqrt = 32,
       GFAbs = 4, GFSign = 6, GFMin = 37, GFMax = 40, GFClamp = 43, GCross = 68, GNormalize = 69 };
enum { SC_UniformConstant = 0, SC_Input = 1, SC_Uniform = 2, SC_Output = 3, SC_Function = 7 };
enum { D_SpecId = 1, D_Block = 2, D_ArrayStride = 6, D_BuiltIn = 11, D_Centroid = 16, D_Location = 30,
       D_Binding = 33, D_DescriptorSet = 34, D_Offset = 35 };
enum { B_Position = 0, B_PointSize = 1, B_FragCoord = 15, B_FrontFacing = 17, B_FragDepth = 22 };

/* ---- registradores do D3D9 ---- */

enum {
    R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_ADDR = 3, R_RASTOUT = 4, R_ATTROUT = 5, R_OUTPUT = 6, R_CONSTINT = 7,
    R_COLOROUT = 8, R_DEPTHOUT = 9, R_SAMPLER = 10, R_CONST2 = 11, R_CONST3 = 12, R_CONST4 = 13, R_CONSTBOOL = 14,
    R_LOOP = 15, R_MISC = 17, R_PREDICATE = 19,
};
enum { USAGE_POSITION = 0, USAGE_PSIZE = 4, USAGE_TEXCOORD = 5, USAGE_COLOR = 10, USAGE_FOG = 11, USAGE_DEPTH = 12 };

#define MAX_TEMP 32
#define MAX_OUT 12
#define MAX_NEST 16

typedef struct {
    int kind;                    /* 1 if, 2 loop/rep */
    uint32_t merge, cont, else_label, header;
    int has_else;
    uint32_t counter, count;     /* loop/rep: variável int do contador e o total */
    uint32_t aL, step;           /* loop: variável do aL e o passo */
} flow;

typedef struct {
    const uint32_t *code;
    size_t ndw, pos, inst_pos;
    int pixel;
    re5vk_dxso_variant var;
    re5vk_shader *out;
    char *err;
    size_t errsz;
    int failed;

    wbuf head, decor, globals, fvars, finit, body;
    uint32_t next_id;
    uint32_t glsl;
    uint32_t t_void, t_bool, t_f32, t_i32, t_u32, t_v2f, t_v3f, t_v4f, t_v4i, t_v4b, t_fn;
    uint32_t k0, k1;             /* vec4(0), vec4(1) */
    uint32_t img_type[2][4], simg_type[2][4];   /* [profundidade][dim] */
    uint32_t ptr_cache[64][3];   /* (classe, tipo) -> id */
    int nptr;
    struct { uint32_t type, bits, id; } k[256];
    int nk;
    uint32_t iface[64];
    int niface;

    uint32_t r[MAX_TEMP];
    uint32_t a0, p0;
    uint32_t vin[16];            /* entradas (variáveis do módulo) */
    uint32_t o[MAX_OUT];         /* VS: saídas (variáveis da função) */
    uint8_t o_usage[MAX_OUT], o_index[MAX_OUT], o_used[MAX_OUT];
    uint32_t oc[4], odepth;      /* PS */
    uint32_t vpos, vface;
    uint32_t ubo, ubo_ptr_v4, ubo_ptr_i4, ubo_ptr_u;
    uint32_t samp[16], samp_type[16];
    float def_f[256][4];
    uint8_t def_f_on[256];
    int32_t def_i[16][4];
    uint8_t def_i_on[16];
    uint32_t def_b, def_b_on;

    flow st[MAX_NEST];
    int nst;
    int open;                    /* o bloco atual ainda não terminou */
    uint32_t alpha_spec;
} cx;

static void fail(cx *c, const char *fmt, ...)
{
    if (c->failed) return;
    c->failed = 1;
    char m[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    snprintf(c->err, c->errsz, "%s, token %zu: %s", c->pixel ? "ps_3_0" : "vs_3_0", c->inst_pos, m);
}

static uint32_t id(cx *c) { return c->next_id++; }

/* ---- tipos e constantes ---- */

static uint32_t t_ptr(cx *c, uint32_t sc, uint32_t type)
{
    for (int i = 0; i < c->nptr; i++)
        if (c->ptr_cache[i][0] == sc && c->ptr_cache[i][1] == type) return c->ptr_cache[i][2];
    uint32_t r = id(c);
    OP(&c->globals, SOpTypePointer, r, sc, type);
    if (c->nptr >= 64) abort();   /* tipo de ponteiro repetido seria SPIR-V inválido */
    {
        c->ptr_cache[c->nptr][0] = sc;
        c->ptr_cache[c->nptr][1] = type;
        c->ptr_cache[c->nptr++][2] = r;
    }
    return r;
}

static uint32_t k_bits(cx *c, uint32_t type, uint32_t bits)
{
    for (int i = 0; i < c->nk; i++)
        if (c->k[i].type == type && c->k[i].bits == bits) return c->k[i].id;
    uint32_t r = id(c);
    OP(&c->globals, SOpConstant, type, r, bits);
    if (c->nk < 256) c->k[c->nk++] = (typeof(c->k[0])){type, bits, r};
    return r;
}
static uint32_t kf(cx *c, float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return k_bits(c, c->t_f32, b);
}
static uint32_t ki(cx *c, int32_t v) { return k_bits(c, c->t_i32, (uint32_t)v); }
static uint32_t ku(cx *c, uint32_t v) { return k_bits(c, c->t_u32, v); }

static uint32_t kv4(cx *c, float x, float y, float z, float w)
{
    if (x == 0 && y == 0 && z == 0 && w == 0 && c->k0) return c->k0;
    if (x == 1 && y == 1 && z == 1 && w == 1 && c->k1) return c->k1;
    uint32_t r = id(c);
    OP(&c->globals, SOpConstantComposite, c->t_v4f, r, kf(c, x), kf(c, y), kf(c, z), kf(c, w));
    return r;
}
static uint32_t kv4i(cx *c, const int32_t v[4])
{
    uint32_t r = id(c);
    OP(&c->globals, SOpConstantComposite, c->t_v4i, r, ki(c, v[0]), ki(c, v[1]), ki(c, v[2]), ki(c, v[3]));
    return r;
}

static void setup_types(cx *c)
{
    c->t_void = id(c); OP(&c->globals, SOpTypeVoid, c->t_void);
    c->t_bool = id(c); OP(&c->globals, SOpTypeBool, c->t_bool);
    c->t_f32 = id(c); OP(&c->globals, SOpTypeFloat, c->t_f32, 32);
    c->t_i32 = id(c); OP(&c->globals, SOpTypeInt, c->t_i32, 32, 1);
    c->t_u32 = id(c); OP(&c->globals, SOpTypeInt, c->t_u32, 32, 0);
    c->t_v2f = id(c); OP(&c->globals, SOpTypeVector, c->t_v2f, c->t_f32, 2);
    c->t_v3f = id(c); OP(&c->globals, SOpTypeVector, c->t_v3f, c->t_f32, 3);
    c->t_v4f = id(c); OP(&c->globals, SOpTypeVector, c->t_v4f, c->t_f32, 4);
    c->t_v4i = id(c); OP(&c->globals, SOpTypeVector, c->t_v4i, c->t_i32, 4);
    c->t_v4b = id(c); OP(&c->globals, SOpTypeVector, c->t_v4b, c->t_bool, 4);
    c->t_fn = id(c); OP(&c->globals, SOpTypeFunction, c->t_fn, c->t_void);
    c->k0 = kv4(c, 0, 0, 0, 0);
    c->k1 = kv4(c, 1, 1, 1, 1);
}

/* ---- instruções no corpo ---- */

static void ensure_open(cx *c)
{
    if (c->open) return;
    uint32_t l = id(c);   /* código depois de um break: bloco morto, ainda estruturado */
    OP(&c->body, SOpLabel, l);
    c->open = 1;
}

static uint32_t e1(cx *c, uint32_t op, uint32_t type, uint32_t a)
{
    ensure_open(c);
    uint32_t r = id(c);
    OP(&c->body, op, type, r, a);
    return r;
}
static uint32_t e2(cx *c, uint32_t op, uint32_t type, uint32_t a, uint32_t b)
{
    ensure_open(c);
    uint32_t r = id(c);
    OP(&c->body, op, type, r, a, b);
    return r;
}
static uint32_t e3(cx *c, uint32_t op, uint32_t type, uint32_t a, uint32_t b, uint32_t d)
{
    ensure_open(c);
    uint32_t r = id(c);
    OP(&c->body, op, type, r, a, b, d);
    return r;
}
static uint32_t gl1(cx *c, uint32_t type, uint32_t f, uint32_t a)
{
    ensure_open(c);
    uint32_t r = id(c);
    OP(&c->body, SOpExtInst, type, r, c->glsl, f, a);
    return r;
}
static uint32_t gl2(cx *c, uint32_t type, uint32_t f, uint32_t a, uint32_t b)
{
    ensure_open(c);
    uint32_t r = id(c);
    OP(&c->body, SOpExtInst, type, r, c->glsl, f, a, b);
    return r;
}
static uint32_t gl3(cx *c, uint32_t type, uint32_t f, uint32_t a, uint32_t b, uint32_t d)
{
    ensure_open(c);
    uint32_t r = id(c);
    OP(&c->body, SOpExtInst, type, r, c->glsl, f, a, b, d);
    return r;
}
static uint32_t load(cx *c, uint32_t type, uint32_t ptr) { return e1(c, SOpLoad, type, ptr); }
static void store(cx *c, uint32_t ptr, uint32_t v)
{
    ensure_open(c);
    OP(&c->body, SOpStore, ptr, v);
}
static uint32_t extract(cx *c, uint32_t type, uint32_t v, uint32_t i) { return e2(c, SOpCompositeExtract, type, v, i); }
static uint32_t splat(cx *c, uint32_t s)
{
    ensure_open(c);
    uint32_t r = id(c);
    OP(&c->body, SOpCompositeConstruct, c->t_v4f, r, s, s, s, s);
    return r;
}
static uint32_t shuffle4(cx *c, uint32_t a, uint32_t b, uint32_t i0, uint32_t i1, uint32_t i2, uint32_t i3)
{
    ensure_open(c);
    uint32_t r = id(c);
    OP(&c->body, SOpVectorShuffle, c->t_v4f, r, a, b, i0, i1, i2, i3);
    return r;
}
static void branch(cx *c, uint32_t to)
{
    ensure_open(c);
    OP(&c->body, SOpBranch, to);
    c->open = 0;
}
/* ---- variáveis ---- */

static uint32_t fvar(cx *c, uint32_t type, uint32_t init)
{
    uint32_t r = id(c);
    OP(&c->fvars, SOpVariable, t_ptr(c, SC_Function, type), r, SC_Function);
    if (init) OP(&c->finit, SOpStore, r, init);
    return r;
}

static uint32_t gvar(cx *c, uint32_t sc, uint32_t type)
{
    uint32_t r = id(c);
    OP(&c->globals, SOpVariable, t_ptr(c, sc, type), r, sc);
    if ((sc == SC_Input || sc == SC_Output) && c->niface < 64) c->iface[c->niface++] = r;
    return r;
}

static uint32_t temp(cx *c, uint32_t n)
{
    if (n >= MAX_TEMP) { fail(c, "r%u", n); return 0; }
    if (!c->r[n]) c->r[n] = fvar(c, c->t_v4f, c->k0);
    return c->r[n];
}

static void setup_ubo(cx *c)
{
    uint32_t nf = c->pixel ? 224u : 256u;
    uint32_t arr_f = id(c), arr_i = id(c), st = id(c);
    OP(&c->globals, SOpTypeArray, arr_f, c->t_v4f, ku(c, nf));
    OP(&c->globals, SOpTypeArray, arr_i, c->t_v4i, ku(c, 16));
    if (c->pixel)
        OP(&c->globals, SOpTypeStruct, st, arr_f, arr_i, c->t_u32, c->t_f32);
    else
        OP(&c->globals, SOpTypeStruct, st, arr_f, arr_i, c->t_u32);
    OP(&c->decor, SOpDecorate, arr_f, D_ArrayStride, 16);
    OP(&c->decor, SOpDecorate, arr_i, D_ArrayStride, 16);
    OP(&c->decor, SOpDecorate, st, D_Block);
    OP(&c->decor, SOpMemberDecorate, st, 0, D_Offset, 0);
    OP(&c->decor, SOpMemberDecorate, st, 1, D_Offset, nf * 16);
    OP(&c->decor, SOpMemberDecorate, st, 2, D_Offset, nf * 16 + 256);
    if (c->pixel) OP(&c->decor, SOpMemberDecorate, st, 3, D_Offset, nf * 16 + 260);
    c->ubo = gvar(c, SC_Uniform, st);
    OP(&c->decor, SOpDecorate, c->ubo, D_DescriptorSet, 0);
    OP(&c->decor, SOpDecorate, c->ubo, D_Binding, c->pixel ? RE5VK_BIND_PS_UBO : RE5VK_BIND_VS_UBO);
    c->ubo_ptr_v4 = t_ptr(c, SC_Uniform, c->t_v4f);
    c->ubo_ptr_i4 = t_ptr(c, SC_Uniform, c->t_v4i);
    c->ubo_ptr_u = t_ptr(c, SC_Uniform, c->t_u32);
}

uint32_t re5vk_dxso_location(uint32_t usage, uint32_t index)
{
    switch (usage) {
    case USAGE_TEXCOORD: return index;          /* 0-9 */
    case USAGE_COLOR: return 10 + index;        /* 10-11 */
    case USAGE_FOG: return 12;
    case 3: return 13 + (index & 1);            /* NORMAL */
    case 6: return 15;                          /* TANGENT */
    case 7: return 16;                          /* BINORMAL */
    case 1: return 17;                          /* BLENDWEIGHT */
    case 2: return 18;                          /* BLENDINDICES */
    case USAGE_POSITION: return 19 + (index & 3); /* POSITION1+ como interpolante */
    case USAGE_DEPTH: return 23;
    default: return 24 + (usage & 7);
    }
}

/* ---- leitura de registradores ---- */

static uint32_t swizzle_of(uint32_t tok) { return (tok >> 16) & 0xFF; }

static uint32_t rel_index(cx *c, uint32_t base, uint32_t reltok)
{
    uint32_t rtype = ((reltok >> 28) & 7) | ((reltok >> 8) & 0x18);
    uint32_t comp = (reltok >> 16) & 3;
    uint32_t iv;
    if (rtype == R_ADDR) {
        if (!c->a0) c->a0 = fvar(c, c->t_v4i, kv4i(c, (int32_t[4]){0, 0, 0, 0}));
        iv = extract(c, c->t_i32, load(c, c->t_v4i, c->a0), comp);
    } else if (rtype == R_LOOP) {
        int i = c->nst;
        while (i > 0 && c->st[i - 1].kind != 2) i--;
        if (!i || !c->st[i - 1].aL) { fail(c, "aL fora de um loop"); return ki(c, 0); }
        iv = load(c, c->t_i32, c->st[i - 1].aL);
    } else {
        fail(c, "endereçamento relativo pelo registrador tipo %u", rtype);
        return ki(c, 0);
    }
    return e2(c, SOpIAdd, c->t_i32, iv, ki(c, (int32_t)base));
}

static uint32_t read_const(cx *c, uint32_t n, uint32_t idx_id)
{
    if (!idx_id && n < 256 && c->def_f_on[n]) return kv4(c, c->def_f[n][0], c->def_f[n][1], c->def_f[n][2], c->def_f[n][3]);
    uint32_t nf = c->pixel ? 224u : 256u;
    if (!idx_id && n >= nf) { fail(c, "c%u fora do limite", n); return kv4(c, 0, 0, 0, 0); }
    uint32_t ix = idx_id ? idx_id : ki(c, (int32_t)n);
    ensure_open(c);
    uint32_t p = id(c);
    OP(&c->body, SOpAccessChain, c->ubo_ptr_v4, p, c->ubo, ki(c, 0), ix);
    return load(c, c->t_v4f, p);
}

static uint32_t read_int(cx *c, uint32_t n)   /* ivec4 */
{
    if (n < 16 && c->def_i_on[n]) return kv4i(c, c->def_i[n]);
    ensure_open(c);
    uint32_t p = id(c);
    OP(&c->body, SOpAccessChain, c->ubo_ptr_i4, p, c->ubo, ki(c, 1), ki(c, (int32_t)n));
    return load(c, c->t_v4i, p);
}

static uint32_t read_bool(cx *c, uint32_t n)
{
    ensure_open(c);
    uint32_t p = id(c);
    OP(&c->body, SOpAccessChain, c->ubo_ptr_u, p, c->ubo, ki(c, 2));
    uint32_t bits = load(c, c->t_u32, p);
    uint32_t bit = e2(c, SOpBitwiseAnd, c->t_u32, e2(c, SOpShiftRightLogical, c->t_u32, bits, ku(c, n)), ku(c, 1));
    return e2(c, SOpINotEqual, c->t_bool, bit, ku(c, 0));
}

static uint32_t bool_const(cx *c, uint32_t n)
{
    if (c->def_b_on & (1u << n)) {
        uint32_t r = id(c);
        OP(&c->globals, (c->def_b >> n) & 1 ? SOpConstantTrue : SOpConstantFalse, c->t_bool, r);
        return r;
    }
    return read_bool(c, n);
}

/* Registrador cru (sem swizzle) como vec4. */
static uint32_t read_reg(cx *c, uint32_t type, uint32_t n, uint32_t idx_id)
{
    switch (type) {
    case R_TEMP: return load(c, c->t_v4f, temp(c, n));
    case R_INPUT:
        if (n >= 16 || !c->vin[n]) { fail(c, "v%u sem dcl", n); return kv4(c, 0, 0, 0, 0); }
        if (idx_id) { fail(c, "v[] relativo"); return kv4(c, 0, 0, 0, 0); }
        return load(c, c->t_v4f, c->vin[n]);
    case R_CONST: return read_const(c, n, idx_id);
    case R_CONST2: case R_CONST3: case R_CONST4: return read_const(c, n + 2048u * (type - R_CONST2 + 1), idx_id);
    case R_CONSTINT: return e1(c, SOpConvertSToF, c->t_v4f, read_int(c, n));
    case R_LOOP: {
        int i = c->nst;
        while (i > 0 && c->st[i - 1].kind != 2) i--;
        if (!i || !c->st[i - 1].aL) { fail(c, "aL fora de um loop"); return kv4(c, 0, 0, 0, 0); }
        return splat(c, e1(c, SOpConvertSToF, c->t_f32, load(c, c->t_i32, c->st[i - 1].aL)));
    }
    case R_MISC:
        if (!c->pixel) break;
        if (n == 0) {   /* vPos: centro do pixel nos inteiros, como no D3D9 */
            if (!c->vpos) {
                c->vpos = gvar(c, SC_Input, c->t_v4f);
                OP(&c->decor, SOpDecorate, c->vpos, D_BuiltIn, B_FragCoord);
            }
            return e2(c, SOpFSub, c->t_v4f, load(c, c->t_v4f, c->vpos), kv4(c, 0.5f, 0.5f, 0, 0));
        }
        if (n == 1) {   /* vFace: +1 de frente, -1 de costas */
            if (!c->vface) {
                c->vface = gvar(c, SC_Input, c->t_bool);
                OP(&c->decor, SOpDecorate, c->vface, D_BuiltIn, B_FrontFacing);
            }
            uint32_t f = e3(c, SOpSelect, c->t_f32, load(c, c->t_bool, c->vface), kf(c, 1), kf(c, -1));
            return splat(c, f);
        }
        break;
    case R_ADDR:
        if (c->pixel) break;
        if (!c->a0) c->a0 = fvar(c, c->t_v4i, kv4i(c, (int32_t[4]){0, 0, 0, 0}));
        return e1(c, SOpConvertSToF, c->t_v4f, load(c, c->t_v4i, c->a0));
    }
    fail(c, "leitura do registrador tipo %u n %u", type, n);
    return kv4(c, 0, 0, 0, 0);
}

static uint32_t apply_swizzle(cx *c, uint32_t v, uint32_t sw)
{
    if (sw == 0xE4) return v;   /* xyzw */
    return shuffle4(c, v, v, sw & 3, (sw >> 2) & 3, (sw >> 4) & 3, (sw >> 6) & 3);
}

/* Fonte completa: registrador, relativo, swizzle e modificador. */
static uint32_t src(cx *c, uint32_t tok, uint32_t reltok)
{
    uint32_t type = ((tok >> 28) & 7) | ((tok >> 8) & 0x18);
    uint32_t n = tok & 0x7FF;
    uint32_t idx = (tok & (1u << 13)) ? rel_index(c, n, reltok) : 0;
    uint32_t v = apply_swizzle(c, read_reg(c, type, n, idx), swizzle_of(tok));
    switch ((tok >> 24) & 0xF) {
    case 0: return v;
    case 1: return e1(c, SOpFNegate, c->t_v4f, v);
    case 2: return e2(c, SOpFSub, c->t_v4f, v, kv4(c, 0.5f, 0.5f, 0.5f, 0.5f));
    case 3: return e1(c, SOpFNegate, c->t_v4f, e2(c, SOpFSub, c->t_v4f, v, kv4(c, 0.5f, 0.5f, 0.5f, 0.5f)));
    case 4: return e2(c, SOpFSub, c->t_v4f, e2(c, SOpFMul, c->t_v4f, v, kv4(c, 2, 2, 2, 2)), kv4(c, 1, 1, 1, 1));
    case 5: return e1(c, SOpFNegate, c->t_v4f,
                      e2(c, SOpFSub, c->t_v4f, e2(c, SOpFMul, c->t_v4f, v, kv4(c, 2, 2, 2, 2)), kv4(c, 1, 1, 1, 1)));
    case 6: return e2(c, SOpFSub, c->t_v4f, kv4(c, 1, 1, 1, 1), v);
    case 7: return e2(c, SOpFMul, c->t_v4f, v, kv4(c, 2, 2, 2, 2));
    case 8: return e1(c, SOpFNegate, c->t_v4f, e2(c, SOpFMul, c->t_v4f, v, kv4(c, 2, 2, 2, 2)));
    case 11: return gl1(c, c->t_v4f, GFAbs, v);
    case 12: return e1(c, SOpFNegate, c->t_v4f, gl1(c, c->t_v4f, GFAbs, v));
    default: fail(c, "modificador de fonte %u", (tok >> 24) & 0xF); return v;
    }
}

/* ---- escrita ---- */

static uint32_t dst_ptr(cx *c, uint32_t tok)
{
    uint32_t type = ((tok >> 28) & 7) | ((tok >> 8) & 0x18);
    uint32_t n = tok & 0x7FF;
    if (tok & (1u << 13)) { fail(c, "destino relativo"); return 0; }
    switch (type) {
    case R_TEMP: return temp(c, n);
    case R_OUTPUT:
        if (c->pixel || n >= MAX_OUT || !c->o[n]) break;
        c->o_used[n] = 1;
        return c->o[n];
    case R_COLOROUT:
        if (!c->pixel || n >= 4) break;
        if (!c->oc[n]) c->oc[n] = fvar(c, c->t_v4f, c->k0);
        return c->oc[n];
    case R_DEPTHOUT:
        if (!c->pixel) break;
        if (!c->odepth) c->odepth = fvar(c, c->t_v4f, c->k0);
        c->out->writes_depth = 1;
        return c->odepth;
    }
    fail(c, "escrita no registrador tipo %u n %u", type, n);
    return 0;
}

static uint32_t pred_mask(cx *c, uint32_t ptok)   /* bvec4 do predicado, com swizzle e not */
{
    if (!c->p0) { fail(c, "predicado sem setp"); return 0; }
    uint32_t p = load(c, c->t_v4b, c->p0);
    uint32_t sw = swizzle_of(ptok);
    if (sw != 0xE4) {
        ensure_open(c);
        uint32_t r = id(c);
        OP(&c->body, SOpVectorShuffle, c->t_v4b, r, p, p, sw & 3, (sw >> 2) & 3, (sw >> 4) & 3, (sw >> 6) & 3);
        p = r;
    }
    if (((ptok >> 24) & 0xF) == 13) p = e1(c, SOpLogicalNot, c->t_v4b, p);
    return p;
}

static void write_dst(cx *c, uint32_t tok, uint32_t v, uint32_t pred)
{
    uint32_t ptr = dst_ptr(c, tok);
    if (!ptr) return;
    uint32_t mod = (tok >> 20) & 0xF;
    if (mod & 1) v = gl3(c, c->t_v4f, GFClamp, v, kv4(c, 0, 0, 0, 0), kv4(c, 1, 1, 1, 1));
    if (mod & ~7u) fail(c, "modificador de destino %u", mod);
    uint32_t mask = (tok >> 16) & 0xF;
    if (mask != 0xF || pred) {
        uint32_t old = load(c, c->t_v4f, ptr);
        if (mask != 0xF)
            v = shuffle4(c, v, old, mask & 1 ? 0 : 4, mask & 2 ? 1 : 5, mask & 4 ? 2 : 6, mask & 8 ? 3 : 7);
        if (pred) v = e3(c, SOpSelect, c->t_v4f, pred, v, old);
    }
    store(c, ptr, v);
}

/* ---- comparações (ifc, breakc, setp) ---- */

static uint32_t compare(cx *c, uint32_t type, uint32_t ctl, uint32_t a, uint32_t b)
{
    static const uint32_t ops[7] = {0, SOpFOrdGreaterThan, SOpFOrdEqual, SOpFOrdGreaterThanEqual, SOpFOrdLessThan,
                                    SOpFOrdNotEqual, SOpFOrdLessThanEqual};
    if (ctl < 1 || ctl > 6) { fail(c, "comparação %u", ctl); return 0; }
    return e2(c, ops[ctl], type, a, b);
}

/* ---- textura ---- */

static uint32_t sample(cx *c, uint32_t s, uint32_t coord, int proj, int bias, int lod, uint32_t dx, uint32_t dy)
{
    if (s >= 16 || !c->samp[s]) { fail(c, "s%u sem dcl", s); return kv4(c, 0, 0, 0, 0); }
    int ttype = c->out->sampler_type[s];
    int depth = c->pixel && (c->var.depth_samplers >> s & 1) && ttype == RE5VK_TEX_2D;
    uint32_t si = load(c, c->samp_type[s], c->samp[s]);
    /* 2D: xy (projetiva: xyw); cubo e volume: xyz */
    ensure_open(c);
    uint32_t co = id(c);
    if (ttype == RE5VK_TEX_2D && proj)
        OP(&c->body, SOpVectorShuffle, c->t_v3f, co, coord, coord, 0, 1, 3);
    else if (ttype == RE5VK_TEX_2D)
        OP(&c->body, SOpVectorShuffle, c->t_v2f, co, coord, coord, 0, 1);
    else
        OP(&c->body, SOpVectorShuffle, c->t_v3f, co, coord, coord, 0, 1, 2);
    uint32_t r = id(c);
    if (depth) {   /* textura de profundidade: comparação com .z (mapa de sombra do D3D9) */
        uint32_t dref = extract(c, c->t_f32, coord, 2);
        if (lod) {
            uint32_t l = extract(c, c->t_f32, coord, 3);
            r = id(c);
            OP(&c->body, SOpImageSampleDrefExplicitLod, c->t_f32, r, si, co, dref, 2, l);
        } else if (proj || !c->pixel) {
            if (!c->pixel) { fail(c, "profundidade no VS"); return c->k0; }
            r = id(c);
            OP(&c->body, SOpImageSampleProjDrefImplicitLod, c->t_f32, r, si, co, dref);
        } else {
            r = id(c);
            OP(&c->body, SOpImageSampleDrefImplicitLod, c->t_f32, r, si, co, dref);
        }
        return splat(c, r);
    }
    if (dx) {
        uint32_t t = ttype == RE5VK_TEX_2D ? c->t_v2f : c->t_v3f;
        uint32_t gx = id(c), gy = id(c);
        if (t == c->t_v2f) {
            OP(&c->body, SOpVectorShuffle, t, gx, dx, dx, 0, 1);
            OP(&c->body, SOpVectorShuffle, t, gy, dy, dy, 0, 1);
        } else {
            OP(&c->body, SOpVectorShuffle, t, gx, dx, dx, 0, 1, 2);
            OP(&c->body, SOpVectorShuffle, t, gy, dy, dy, 0, 1, 2);
        }
        r = id(c);
        OP(&c->body, SOpImageSampleExplicitLod, c->t_v4f, r, si, co, 4, gx, gy);
    } else if (lod || !c->pixel) {
        uint32_t l = lod ? extract(c, c->t_f32, coord, 3) : kf(c, 0);
        r = id(c);
        OP(&c->body, SOpImageSampleExplicitLod, c->t_v4f, r, si, co, 2, l);
    } else if (proj) {
        OP(&c->body, SOpImageSampleProjImplicitLod, c->t_v4f, r, si, co);
    } else if (bias) {
        uint32_t b = extract(c, c->t_f32, coord, 3);
        r = id(c);
        OP(&c->body, SOpImageSampleImplicitLod, c->t_v4f, r, si, co, 1, b);
    } else {
        OP(&c->body, SOpImageSampleImplicitLod, c->t_v4f, r, si, co);
    }
    return r;
}

/* ---- declarações ---- */

static void do_dcl(cx *c, uint32_t dcl, uint32_t reg)
{
    uint32_t type = ((reg >> 28) & 7) | ((reg >> 8) & 0x18);
    uint32_t n = reg & 0x7FF;
    uint32_t usage = dcl & 0xF, index = (dcl >> 16) & 0xF;
    if (type == R_SAMPLER) {
        if (n >= (c->pixel ? 16u : 4u)) { fail(c, "s%u", n); return; }
        uint32_t tt = (dcl >> 27) & 0xF;
        uint32_t dim = tt == RE5VK_TEX_2D ? 1 : tt == RE5VK_TEX_CUBE ? 3 : tt == RE5VK_TEX_3D ? 2 : 99;
        if (dim == 99) { fail(c, "sampler tipo %u", tt); return; }
        int depth = c->pixel && (c->var.depth_samplers >> n & 1) && tt == RE5VK_TEX_2D;
        uint32_t *img = &c->img_type[depth][dim], *simg = &c->simg_type[depth][dim];
        if (!*img) {
            *img = id(c);
            *simg = id(c);
            OP(&c->globals, SOpTypeImage, *img, c->t_f32, dim, depth ? 1 : 0, 0, 0, 1, 0);
            OP(&c->globals, SOpTypeSampledImage, *simg, *img);
        }
        c->samp_type[n] = *simg;
        c->samp[n] = gvar(c, SC_UniformConstant, *simg);
        OP(&c->decor, SOpDecorate, c->samp[n], D_DescriptorSet, 0);
        OP(&c->decor, SOpDecorate, c->samp[n], D_Binding, (c->pixel ? RE5VK_BIND_PS_SAMP : RE5VK_BIND_VS_SAMP) + n);
        c->out->samplers |= (uint16_t)(1u << n);
        c->out->sampler_type[n] = (uint8_t)tt;
        return;
    }
    if (type == R_INPUT) {
        if (n >= 16) { fail(c, "v%u", n); return; }
        c->vin[n] = gvar(c, SC_Input, c->t_v4f);
        uint32_t loc = c->pixel ? re5vk_dxso_location(usage, index) : n;
        OP(&c->decor, SOpDecorate, c->vin[n], D_Location, loc);
        if (c->pixel && ((reg >> 20) & 4)) OP(&c->decor, SOpDecorate, c->vin[n], D_Centroid);
        if (!c->pixel) {
            c->out->inputs |= (uint16_t)(1u << n);
            c->out->in_usage[n] = (uint8_t)usage;
            c->out->in_index[n] = (uint8_t)index;
        }
        return;
    }
    if (type == R_OUTPUT && !c->pixel) {
        if (n >= MAX_OUT) { fail(c, "o%u", n); return; }
        c->o[n] = fvar(c, c->t_v4f, c->k0);
        c->o_usage[n] = (uint8_t)usage;
        c->o_index[n] = (uint8_t)index;
        return;
    }
    if (type == R_MISC && c->pixel) return;   /* vPos/vFace: criados na primeira leitura */
    fail(c, "dcl do registrador tipo %u", type);
}

/* ---- fim do shader: saídas do Vulkan ---- */

static void finish_outputs(cx *c)
{
    if (!c->pixel) {
        for (int n = 0; n < MAX_OUT; n++) {
            if (!c->o[n]) continue;
            uint32_t v = load(c, c->t_v4f, c->o[n]);
            if (c->o_usage[n] == USAGE_POSITION && c->o_index[n] == 0) {
                uint32_t pos = gvar(c, SC_Output, c->t_v4f);
                OP(&c->decor, SOpDecorate, pos, D_BuiltIn, B_Position);
                store(c, pos, v);
            } else if (c->o_usage[n] == USAGE_PSIZE) {
                uint32_t ps = gvar(c, SC_Output, c->t_f32);
                OP(&c->decor, SOpDecorate, ps, D_BuiltIn, B_PointSize);
                store(c, ps, extract(c, c->t_f32, v, 0));
            } else {
                uint32_t ov = gvar(c, SC_Output, c->t_v4f);
                OP(&c->decor, SOpDecorate, ov, D_Location, re5vk_dxso_location(c->o_usage[n], c->o_index[n]));
                store(c, ov, v);
            }
        }
        return;
    }
    /* alpha test do D3D9 (D3DRS_ALPHAFUNC na variante, referência no UBO) */
    if (c->var.alpha_func && c->var.alpha_func != 8 && c->oc[0]) {
        uint32_t a = extract(c, c->t_f32, load(c, c->t_v4f, c->oc[0]), 3);
        uint32_t p = id(c);
        ensure_open(c);
        OP(&c->body, SOpAccessChain, t_ptr(c, SC_Uniform, c->t_f32), p, c->ubo, ki(c, 3));
        uint32_t ref = load(c, c->t_f32, p);
        uint32_t pass;
        switch (c->var.alpha_func) {
        case 1: pass = 0; break;   /* NEVER */
        case 2: pass = e2(c, SOpFOrdLessThan, c->t_bool, a, ref); break;
        case 3: pass = e2(c, SOpFOrdEqual, c->t_bool, a, ref); break;
        case 4: pass = e2(c, SOpFOrdLessThanEqual, c->t_bool, a, ref); break;
        case 5: pass = e2(c, SOpFOrdGreaterThan, c->t_bool, a, ref); break;
        case 6: pass = e2(c, SOpFOrdNotEqual, c->t_bool, a, ref); break;
        case 7: pass = e2(c, SOpFOrdGreaterThanEqual, c->t_bool, a, ref); break;
        default: fail(c, "alpha func %u", c->var.alpha_func); return;
        }
        uint32_t kill = id(c), keep = id(c);
        if (pass) {
            OP(&c->body, SOpSelectionMerge, keep, 0);
            OP(&c->body, SOpBranchConditional, pass, keep, kill);
        } else {
            OP(&c->body, SOpBranch, kill);
        }
        OP(&c->body, SOpLabel, kill);
        OP0(&c->body, SOpKill);
        OP(&c->body, SOpLabel, keep);
        if (!pass) { /* NEVER: o bloco keep não é alcançado, mas precisa terminar */ }
        c->open = 1;
    }
    for (int n = 0; n < 4; n++) {
        if (!c->oc[n]) continue;
        uint32_t ov = gvar(c, SC_Output, c->t_v4f);
        OP(&c->decor, SOpDecorate, ov, D_Location, n);
        store(c, ov, load(c, c->t_v4f, c->oc[n]));
    }
    if (c->odepth) {
        uint32_t od = gvar(c, SC_Output, c->t_f32);
        OP(&c->decor, SOpDecorate, od, D_BuiltIn, B_FragDepth);
        store(c, od, extract(c, c->t_f32, load(c, c->t_v4f, c->odepth), 0));
    }
}

/* ---- fluxo de controle ---- */

static void if_begin(cx *c, uint32_t cond)
{
    if (c->nst >= MAX_NEST) { fail(c, "aninhamento"); return; }
    flow *f = &c->st[c->nst++];
    memset(f, 0, sizeof *f);
    f->kind = 1;
    f->merge = id(c);
    f->else_label = id(c);
    uint32_t then = id(c);
    ensure_open(c);
    OP(&c->body, SOpSelectionMerge, f->merge, 0);
    OP(&c->body, SOpBranchConditional, cond, then, f->else_label);
    OP(&c->body, SOpLabel, then);
    c->open = 1;
}

static void if_else(cx *c)
{
    if (!c->nst || c->st[c->nst - 1].kind != 1 || c->st[c->nst - 1].has_else) { fail(c, "else sem if"); return; }
    flow *f = &c->st[c->nst - 1];
    if (c->open) branch(c, f->merge);
    OP(&c->body, SOpLabel, f->else_label);
    c->open = 1;
    f->has_else = 1;
}

static void if_end(cx *c)
{
    if (!c->nst || c->st[c->nst - 1].kind != 1) { fail(c, "endif sem if"); return; }
    flow *f = &c->st[--c->nst];
    if (c->open) branch(c, f->merge);
    if (!f->has_else) {
        OP(&c->body, SOpLabel, f->else_label);
        OP(&c->body, SOpBranch, f->merge);
    }
    OP(&c->body, SOpLabel, f->merge);
    c->open = 1;
}

/* loop aL, i# (com aL) ou rep i# (sem): contador de 0 a i.x. */
static void loop_begin(cx *c, uint32_t itok, int with_al)
{
    if (c->nst >= MAX_NEST) { fail(c, "aninhamento"); return; }
    uint32_t ivec = read_int(c, itok & 0x7FF);
    flow *f = &c->st[c->nst];
    memset(f, 0, sizeof *f);
    f->kind = 2;
    f->counter = fvar(c, c->t_i32, 0);
    store(c, f->counter, ki(c, 0));
    f->count = extract(c, c->t_i32, ivec, 0);
    if (with_al) {
        f->aL = fvar(c, c->t_i32, 0);
        store(c, f->aL, extract(c, c->t_i32, ivec, 1));
        f->step = extract(c, c->t_i32, ivec, 2);
    }
    c->nst++;
    f->header = id(c);
    f->merge = id(c);
    f->cont = id(c);
    uint32_t bodyl = id(c);
    branch(c, f->header);
    OP(&c->body, SOpLabel, f->header);
    c->open = 1;
    uint32_t cnt = load(c, c->t_i32, f->counter);
    uint32_t go = e2(c, SOpSLessThan, c->t_bool, cnt, f->count);
    OP(&c->body, SOpLoopMerge, f->merge, f->cont, 0);
    OP(&c->body, SOpBranchConditional, go, bodyl, f->merge);
    OP(&c->body, SOpLabel, bodyl);
}

static void loop_end(cx *c)
{
    if (!c->nst || c->st[c->nst - 1].kind != 2) { fail(c, "endloop sem loop"); return; }
    flow *f = &c->st[--c->nst];
    if (c->open) branch(c, f->cont);
    OP(&c->body, SOpLabel, f->cont);
    c->open = 1;
    store(c, f->counter, e2(c, SOpIAdd, c->t_i32, load(c, c->t_i32, f->counter), ki(c, 1)));
    if (f->aL) store(c, f->aL, e2(c, SOpIAdd, c->t_i32, load(c, c->t_i32, f->aL), f->step));
    branch(c, f->header);
    OP(&c->body, SOpLabel, f->merge);
    c->open = 1;
}

static void do_break(cx *c, uint32_t cond)   /* cond 0 = incondicional */
{
    int i = c->nst;
    while (i > 0 && c->st[i - 1].kind != 2) i--;
    if (!i) { fail(c, "break fora de loop"); return; }
    uint32_t to = c->st[i - 1].merge;
    if (!cond) {
        branch(c, to);
        return;
    }
    uint32_t next = id(c);
    ensure_open(c);
    OP(&c->body, SOpSelectionMerge, next, 0);
    OP(&c->body, SOpBranchConditional, cond, to, next);
    OP(&c->body, SOpLabel, next);
    c->open = 1;
}

/* ---- instruções ---- */

static uint32_t dot(cx *c, uint32_t a, uint32_t b, int n)
{
    if (n == 4) return e2(c, SOpDot, c->t_f32, a, b);
    uint32_t t = n == 3 ? c->t_v3f : c->t_v2f;
    uint32_t x = id(c), y = id(c);
    ensure_open(c);
    if (n == 3) {
        OP(&c->body, SOpVectorShuffle, t, x, a, a, 0, 1, 2);
        OP(&c->body, SOpVectorShuffle, t, y, b, b, 0, 1, 2);
    } else {
        OP(&c->body, SOpVectorShuffle, t, x, a, a, 0, 1);
        OP(&c->body, SOpVectorShuffle, t, y, b, b, 0, 1);
    }
    return e2(c, SOpDot, c->t_f32, x, y);
}

static uint32_t x_of(cx *c, uint32_t v) { return extract(c, c->t_f32, v, 0); }

/* mNxM: dst.i = dot(src0, src1 + i) para i < rows */
static uint32_t matrix(cx *c, uint32_t s0, uint32_t s1tok, uint32_t reltok, int n, int rows)
{
    uint32_t comp[4];
    for (int i = 0; i < 4; i++) comp[i] = kf(c, 0);
    for (int i = 0; i < rows; i++) {
        uint32_t row = src(c, s1tok + (uint32_t)i, reltok);   /* registrador seguinte */
        comp[i] = dot(c, s0, row, n);
    }
    ensure_open(c);
    uint32_t r = id(c);
    OP(&c->body, SOpCompositeConstruct, c->t_v4f, r, comp[0], comp[1], comp[2], comp[3]);
    return r;
}

static int translate(cx *c)
{
    const uint32_t *t = c->code;
    size_t i = 1;
    while (i < c->ndw && !c->failed) {
        uint32_t tok = t[i];
        c->inst_pos = i;
        uint32_t op = tok & 0xFFFF;
        if (op == 0xFFFF) return 0;
        if (op == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        uint32_t len = (tok >> 24) & 0xF, ctl = (tok >> 16) & 0xFF;
        int predicated = (tok >> 28) & 1;
        const uint32_t *a = &t[i + 1];
        size_t next = i + 1 + len;
        if (next > c->ndw) { fail(c, "instrução passa do fim"); break; }
        i = next;
        if (op == 31) { do_dcl(c, a[0], a[1]); continue; }
        if (op == 81) {   /* def c#, x, y, z, w */
            uint32_t n = a[0] & 0x7FF;
            if (n < 256) { memcpy(c->def_f[n], &a[1], 16); c->def_f_on[n] = 1; }
            continue;
        }
        if (op == 48) {   /* defi */
            uint32_t n = a[0] & 0x7FF;
            if (n < 16) { memcpy(c->def_i[n], &a[1], 16); c->def_i_on[n] = 1; }
            continue;
        }
        if (op == 47) {   /* defb */
            uint32_t n = a[0] & 0x7FF;
            if (n < 32) { c->def_b_on |= 1u << n; if (a[1]) c->def_b |= 1u << n; else c->def_b &= ~(1u << n); }
            continue;
        }
        if (op == 0) continue;   /* nop */

        /* operandos: destino [+relativo], predicado, fontes [+relativo cada] */
        uint32_t dtok = 0, ptok = 0, s[4] = {0}, rel[4] = {0};
        int ns = 0;
        size_t k = 0;
        static const uint8_t has_dst[100] = {
            [1] = 1, [2] = 1, [3] = 1, [4] = 1, [5] = 1, [6] = 1, [7] = 1, [8] = 1, [9] = 1, [10] = 1, [11] = 1,
            [12] = 1, [13] = 1, [14] = 1, [15] = 1, [16] = 1, [17] = 1, [18] = 1, [19] = 1, [20] = 1, [21] = 1,
            [22] = 1, [23] = 1, [24] = 1, [32] = 1, [33] = 1, [34] = 1, [35] = 1, [36] = 1, [37] = 1, [46] = 1,
            [65] = 1, [66] = 1, [78] = 1, [79] = 1, [88] = 1, [90] = 1, [91] = 1, [92] = 1, [93] = 1, [94] = 1,
            [95] = 1};
        if (op < 100 && has_dst[op]) {
            dtok = a[k++];
            if (dtok & (1u << 13)) k++;   /* SM3: token relativo do destino */
        }
        if (predicated) ptok = a[k++];
        while (k < len && ns < 4) {
            s[ns] = a[k++];
            if ((s[ns] & (1u << 13)) && k < len) rel[ns] = a[k++];
            ns++;
        }
        uint32_t pred = predicated ? pred_mask(c, ptok) : 0;
        uint32_t v0, v1, v2, r = 0;
#define S(n) src(c, s[n], rel[n])
        switch (op) {
        case 1: r = S(0); break;   /* mov */
        case 2: r = e2(c, SOpFAdd, c->t_v4f, S(0), S(1)); break;
        case 3: r = e2(c, SOpFSub, c->t_v4f, S(0), S(1)); break;
        case 4: v0 = S(0); v1 = S(1); v2 = S(2); r = e2(c, SOpFAdd, c->t_v4f, e2(c, SOpFMul, c->t_v4f, v0, v1), v2); break;
        case 5: r = e2(c, SOpFMul, c->t_v4f, S(0), S(1)); break;
        case 6: r = splat(c, e2(c, SOpFDiv, c->t_f32, kf(c, 1), x_of(c, S(0)))); break;   /* rcp */
        case 7: r = splat(c, gl1(c, c->t_f32, GInverseSqrt, gl1(c, c->t_f32, GFAbs, x_of(c, S(0))))); break;
        case 8: v0 = S(0); r = splat(c, dot(c, v0, S(1), 3)); break;
        case 9: v0 = S(0); r = splat(c, dot(c, v0, S(1), 4)); break;
        case 10: r = gl2(c, c->t_v4f, GFMin, S(0), S(1)); break;
        case 11: r = gl2(c, c->t_v4f, GFMax, S(0), S(1)); break;
        case 12: case 13: {   /* slt / sge */
            v0 = S(0); v1 = S(1);
            uint32_t b = e2(c, op == 12 ? SOpFOrdLessThan : SOpFOrdGreaterThanEqual, c->t_v4b, v0, v1);
            r = e3(c, SOpSelect, c->t_v4f, b, kv4(c, 1, 1, 1, 1), kv4(c, 0, 0, 0, 0));
            break;
        }
        case 14: case 78: r = splat(c, gl1(c, c->t_f32, GExp2, x_of(c, S(0)))); break;   /* exp, expp */
        case 15: case 79: r = splat(c, gl1(c, c->t_f32, GLog2, gl1(c, c->t_f32, GFAbs, x_of(c, S(0))))); break;
        case 16: {   /* lit */
            v0 = S(0);
            uint32_t x = extract(c, c->t_f32, v0, 0), y = extract(c, c->t_f32, v0, 1), w = extract(c, c->t_f32, v0, 3);
            uint32_t w2 = gl3(c, c->t_f32, GFClamp, w, kf(c, -127.9961f), kf(c, 127.9961f));
            uint32_t dy = gl2(c, c->t_f32, GFMax, x, kf(c, 0));
            uint32_t pz = gl2(c, c->t_f32, GPow, gl2(c, c->t_f32, GFMax, y, kf(c, 0)), w2);
            uint32_t xp = e2(c, SOpFOrdGreaterThan, c->t_bool, x, kf(c, 0));
            uint32_t yp = e2(c, SOpFOrdGreaterThan, c->t_bool, y, kf(c, 0));
            uint32_t z = e3(c, SOpSelect, c->t_f32, e2(c, SOpLogicalAnd, c->t_bool, xp, yp), pz, kf(c, 0));
            ensure_open(c);
            r = id(c);
            OP(&c->body, SOpCompositeConstruct, c->t_v4f, r, kf(c, 1), dy, z, kf(c, 1));
            break;
        }
        case 17: {   /* dst */
            v0 = S(0); v1 = S(1);
            uint32_t y = e2(c, SOpFMul, c->t_f32, extract(c, c->t_f32, v0, 1), extract(c, c->t_f32, v1, 1));
            ensure_open(c);
            r = id(c);
            OP(&c->body, SOpCompositeConstruct, c->t_v4f, r, kf(c, 1), y, extract(c, c->t_f32, v0, 2),
               extract(c, c->t_f32, v1, 3));
            break;
        }
        case 18: {   /* lrp: s2 + s0 * (s1 - s2) */
            v0 = S(0); v1 = S(1); v2 = S(2);
            r = e2(c, SOpFAdd, c->t_v4f, v2, e2(c, SOpFMul, c->t_v4f, v0, e2(c, SOpFSub, c->t_v4f, v1, v2)));
            break;
        }
        case 19: r = gl1(c, c->t_v4f, GFract, S(0)); break;
        case 20: v0 = S(0); r = matrix(c, v0, s[1], rel[1], 4, 4); break;   /* m4x4 */
        case 21: v0 = S(0); r = matrix(c, v0, s[1], rel[1], 4, 3); break;   /* m4x3 */
        case 22: v0 = S(0); r = matrix(c, v0, s[1], rel[1], 3, 4); break;   /* m3x4 */
        case 23: v0 = S(0); r = matrix(c, v0, s[1], rel[1], 3, 3); break;   /* m3x3 */
        case 24: v0 = S(0); r = matrix(c, v0, s[1], rel[1], 3, 2); break;   /* m3x2 */
        case 27: loop_begin(c, s[1], 1); continue;
        case 28:   /* ret: no main é o fim (depois viriam sub-rotinas, sem tradução: call aborta antes) */
            if (!c->nst) return 0;
            fail(c, "ret dentro de if/loop");
            continue;
        case 29: loop_end(c); continue;
        case 32: {   /* pow |x|^y */
            v0 = S(0); v1 = S(1);
            r = splat(c, gl2(c, c->t_f32, GPow, gl1(c, c->t_f32, GFAbs, x_of(c, v0)), x_of(c, v1)));
            break;
        }
        case 33: {   /* crs */
            v0 = S(0); v1 = S(1);
            uint32_t a3 = id(c), b3 = id(c);
            ensure_open(c);
            OP(&c->body, SOpVectorShuffle, c->t_v3f, a3, v0, v0, 0, 1, 2);
            OP(&c->body, SOpVectorShuffle, c->t_v3f, b3, v1, v1, 0, 1, 2);
            uint32_t x3 = gl2(c, c->t_v3f, GCross, a3, b3);
            r = id(c);
            OP(&c->body, SOpVectorShuffle, c->t_v4f, r, x3, x3, 0, 1, 2, 2);
            break;
        }
        case 34: r = gl1(c, c->t_v4f, GFSign, S(0)); break;
        case 35: r = gl1(c, c->t_v4f, GFAbs, S(0)); break;
        case 36: {   /* nrm: xyz / |xyz|, w = 1/|xyz| * w */
            v0 = S(0);
            uint32_t inv = gl1(c, c->t_f32, GInverseSqrt, dot(c, v0, v0, 3));
            r = e2(c, SOpVectorTimesScalar, c->t_v4f, v0, inv);
            break;
        }
        case 37: {   /* sincos: x = cos, y = sin */
            v0 = S(0);
            uint32_t x = x_of(c, v0);
            ensure_open(c);
            uint32_t cs = gl1(c, c->t_f32, GCos, x), sn = gl1(c, c->t_f32, GSin, x);
            r = id(c);
            OP(&c->body, SOpCompositeConstruct, c->t_v4f, r, cs, sn, kf(c, 0), kf(c, 0));
            break;
        }
        case 38: loop_begin(c, s[0], 0); continue;   /* rep */
        case 39: loop_end(c); continue;
        case 40: {   /* if b# / if p0.x */
            uint32_t type = ((s[0] >> 28) & 7) | ((s[0] >> 8) & 0x18);
            uint32_t cond;
            if (type == R_CONSTBOOL) cond = bool_const(c, s[0] & 0x7FF);
            else if (type == R_PREDICATE) {
                uint32_t pm = pred_mask(c, s[0]);
                cond = e2(c, SOpCompositeExtract, c->t_bool, pm, 0);
            } else { fail(c, "if de registrador tipo %u", type); continue; }
            if_begin(c, cond);
            continue;
        }
        case 41: v0 = S(0); v1 = S(1); if_begin(c, compare(c, c->t_bool, ctl, x_of(c, v0), x_of(c, v1))); continue;
        case 42: if_else(c); continue;
        case 43: if_end(c); continue;
        case 44: do_break(c, 0); continue;
        case 45: v0 = S(0); v1 = S(1); do_break(c, compare(c, c->t_bool, ctl, x_of(c, v0), x_of(c, v1))); continue;
        case 96: {   /* breakp p0.x */
            uint32_t pm = pred_mask(c, s[0]);
            do_break(c, e2(c, SOpCompositeExtract, c->t_bool, pm, 0));
            continue;
        }
        case 46: {   /* mova: arredonda para o inteiro mais próximo */
            v0 = S(0);
            if (!c->a0) c->a0 = fvar(c, c->t_v4i, kv4i(c, (int32_t[4]){0, 0, 0, 0}));
            uint32_t f = gl1(c, c->t_v4f, GFloor, e2(c, SOpFAdd, c->t_v4f, v0, kv4(c, 0.5f, 0.5f, 0.5f, 0.5f)));
            uint32_t iv = e1(c, SOpConvertFToS, c->t_v4i, f);
            uint32_t mask = (dtok >> 16) & 0xF;
            if (mask != 0xF) {
                uint32_t old = load(c, c->t_v4i, c->a0);
                ensure_open(c);
                uint32_t m = id(c);
                OP(&c->body, SOpVectorShuffle, c->t_v4i, m, iv, old, mask & 1 ? 0 : 4, mask & 2 ? 1 : 5,
                   mask & 4 ? 2 : 6, mask & 8 ? 3 : 7);
                iv = m;
            }
            store(c, c->a0, iv);
            continue;
        }
        case 65: {   /* texkill: descarta se algum componente da máscara for < 0 */
            uint32_t type = ((dtok >> 28) & 7) | ((dtok >> 8) & 0x18);
            uint32_t v = read_reg(c, type, dtok & 0x7FF, 0);
            uint32_t lt = e2(c, SOpFOrdLessThan, c->t_v4b, v, c->k0);
            uint32_t mask = (dtok >> 16) & 0xF, any = 0;
            for (uint32_t k2 = 0; k2 < 4; k2++) {
                if (!(mask >> k2 & 1)) continue;
                uint32_t b = e2(c, SOpCompositeExtract, c->t_bool, lt, k2);
                any = any ? e2(c, SOpLogicalOr, c->t_bool, any, b) : b;
            }
            if (!any) continue;
            uint32_t kill = id(c), keep = id(c);
            OP(&c->body, SOpSelectionMerge, keep, 0);
            OP(&c->body, SOpBranchConditional, any, kill, keep);
            OP(&c->body, SOpLabel, kill);
            OP0(&c->body, SOpKill);
            OP(&c->body, SOpLabel, keep);
            c->open = 1;
            continue;
        }
        case 66: v0 = S(0); r = sample(c, s[1] & 0x7FF, v0, ctl == 1, ctl == 2, 0, 0, 0); break;   /* texld */
        case 95: v0 = S(0); r = sample(c, s[1] & 0x7FF, v0, 0, 0, 1, 0, 0); break;   /* texldl */
        case 93: {   /* texldd */
            v0 = S(0); uint32_t ddx = S(2), ddy = S(3);
            r = sample(c, s[1] & 0x7FF, v0, 0, 0, 0, ddx, ddy);
            break;
        }
        case 88: {   /* cmp: s0 >= 0 ? s1 : s2 */
            v0 = S(0); v1 = S(1); v2 = S(2);
            uint32_t b = e2(c, SOpFOrdGreaterThanEqual, c->t_v4b, v0, kv4(c, 0, 0, 0, 0));
            r = e3(c, SOpSelect, c->t_v4f, b, v1, v2);
            break;
        }
        case 90: {   /* dp2add */
            v0 = S(0); v1 = S(1); v2 = S(2);
            r = splat(c, e2(c, SOpFAdd, c->t_f32, dot(c, v0, v1, 2), x_of(c, v2)));
            break;
        }
        case 91: r = e1(c, SOpDPdx, c->t_v4f, S(0)); break;
        case 92: r = e1(c, SOpDPdy, c->t_v4f, S(0)); break;
        case 94: {   /* setp_comp p0, s0, s1 */
            v0 = S(0); v1 = S(1);
            uint32_t b = compare(c, c->t_v4b, ctl, v0, v1);
            if (!c->p0) c->p0 = fvar(c, c->t_v4b, 0);
            uint32_t mask = (dtok >> 16) & 0xF;
            if (mask != 0xF) {
                uint32_t old = load(c, c->t_v4b, c->p0);
                ensure_open(c);
                uint32_t m = id(c);
                OP(&c->body, SOpVectorShuffle, c->t_v4b, m, b, old, mask & 1 ? 0 : 4, mask & 2 ? 1 : 5,
                   mask & 4 ? 2 : 6, mask & 8 ? 3 : 7);
                b = m;
            }
            store(c, c->p0, b);
            continue;
        }
        default:
            fail(c, "opcode %u sem tradução", op);
            continue;
        }
#undef S
        if (dtok && r) write_dst(c, dtok, r, pred);
    }
    if (!c->failed) fail(c, "sem token de fim");
    return -1;
}

int re5vk_dxso_compile(const uint32_t *code, size_t ndw, const re5vk_dxso_variant *var, re5vk_shader *out, char *err,
                       size_t errsz)
{
    memset(out, 0, sizeof *out);
    if (errsz) err[0] = 0;
    if (ndw < 2) {
        snprintf(err, errsz, "bytecode vazio");
        return -1;
    }
    uint32_t ver = code[0];
    int pixel = (ver >> 16) == 0xFFFF;
    if ((ver >> 16) != 0xFFFE && !pixel) { snprintf(err, errsz, "token de versão 0x%08X", ver); return -1; }
    if ((ver & 0xFFFF) != 0x0300) { snprintf(err, errsz, "%s_%u_%u: só SM3", pixel ? "ps" : "vs", (ver >> 8) & 0xFF, ver & 0xFF); return -1; }

    cx *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->code = code;
    c->ndw = ndw;
    c->pixel = pixel;
    c->var = var ? *var : (re5vk_dxso_variant){0, 0};
    c->out = out;
    c->err = err;
    c->errsz = errsz;
    c->next_id = 1;
    c->open = 1;   /* o bloco de entrada (o OpLabel sai na montagem) */
    out->pixel = pixel;

    c->glsl = id(c);
    uint32_t mainf = id(c);
    setup_types(c);
    setup_ubo(c);
    translate(c);
    if (!c->failed && c->nst) fail(c, "if/loop sem fim");
    if (!c->failed) finish_outputs(c);
    if (c->failed) {
        free(c->head.w); free(c->decor.w); free(c->globals.w); free(c->fvars.w); free(c->finit.w); free(c->body.w);
        free(c);
        return -1;
    }

    wbuf m = {0};
    put(&m, 0x07230203u);
    put(&m, 0x00010000u);   /* SPIR-V 1.0 */
    put(&m, 0);
    put(&m, c->next_id);    /* bound: ajustado no fim */
    put(&m, 0);
    OP(&m, SOpCapability, 1 /* Shader */);
    {
        wbuf s = {0};
        put(&s, c->glsl);
        put_str(&s, "GLSL.std.450");
        op_emit(&m, SOpExtInstImport, s.w, s.n);
        free(s.w);
    }
    OP(&m, SOpMemoryModel, 0 /* Logical */, 1 /* GLSL450 */);
    {
        wbuf s = {0};
        put(&s, pixel ? 4 : 0);
        put(&s, mainf);
        put_str(&s, "main");
        for (int i = 0; i < c->niface; i++) put(&s, c->iface[i]);
        op_emit(&m, SOpEntryPoint, s.w, s.n);
        free(s.w);
    }
    if (pixel) {
        OP(&m, SOpExecutionMode, mainf, 7 /* OriginUpperLeft */);
        if (out->writes_depth) OP(&m, SOpExecutionMode, mainf, 12 /* DepthReplacing */);
    }
    for (size_t i = 0; i < c->decor.n; i++) put(&m, c->decor.w[i]);
    for (size_t i = 0; i < c->globals.n; i++) put(&m, c->globals.w[i]);
    OP(&m, SOpFunction, c->t_void, mainf, 0, c->t_fn);
    uint32_t entry = c->next_id++;
    OP(&m, SOpLabel, entry);
    for (size_t i = 0; i < c->fvars.n; i++) put(&m, c->fvars.w[i]);
    for (size_t i = 0; i < c->finit.n; i++) put(&m, c->finit.w[i]);
    for (size_t i = 0; i < c->body.n; i++) put(&m, c->body.w[i]);
    if (!c->open) OP(&m, SOpLabel, c->next_id++);
    OP0(&m, SOpReturn);
    OP0(&m, SOpFunctionEnd);
    m.w[3] = c->next_id;
    out->spv = m.w;
    out->nwords = m.n;
    free(c->head.w); free(c->decor.w); free(c->globals.w); free(c->fvars.w); free(c->finit.w); free(c->body.w);
    free(c);
    return 0;
}
