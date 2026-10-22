/*
 * kernel32_nls.c - code pages e locale (Fase 3, etapa 3.5).
 *
 * O guest vê um Windows em inglês dos EUA: ANSI = 1252, OEM = 437, LCID 0x0409.
 * É o que o CRT do MSVC 2005 consulta para montar as tabelas de ctype/mbctype e
 * o que o jogo usa para converter caminhos e textos. Conversões cobrem 1252
 * (= Latin-1 com a faixa 0x80-0x9F do Windows), 437 só na faixa ASCII, e UTF-8.
 * Code page ou tipo de informação que não está aqui aborta (D8).
 */
#include <stdio.h>
#include <stdlib.h>

#include "kernel32_internal.h"

#define CP_ACP     0u
#define CP_OEMCP   1u
#define CP_THREAD  3u
#define CP_UTF8    65001u
#define ACP        1252u
#define OEMCP      437u
#define LANG_ENUS  0x0409u

/* 0x80-0x9F do cp1252 (0 = não mapeado). */
static const uint16_t k_1252_hi[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

static uint32_t resolve_cp(uint32_t cp)
{
    if (cp == CP_ACP || cp == CP_THREAD) return ACP;
    if (cp == CP_OEMCP) return OEMCP;
    if (cp == ACP || cp == OEMCP || cp == CP_UTF8 || cp == 20127u || cp == 28591u) return cp;
    re5_fatal("kernel32", "code page %u sem implementação", cp);
}

static uint16_t sb_to_wide(uint32_t cp, uint8_t b)
{
    if (b < 0x80) return b;
    if (cp == ACP) return b < 0xA0 ? (k_1252_hi[b - 0x80] ? k_1252_hi[b - 0x80] : b) : b;
    if (cp == 28591u) return b;
    return 0xFFFD;   /* 437/20127 fora do ASCII: não usado pelo jogo até agora */
}

static int wide_to_sb(uint32_t cp, uint16_t w)
{
    if (w < 0x80) return w;
    if (cp == ACP) {
        if (w >= 0xA0 && w <= 0xFF) return w;
        for (int i = 0; i < 32; i++)
            if (k_1252_hi[i] == w) return 0x80 + i;
        return -1;
    }
    if (cp == 28591u && w <= 0xFF) return w;
    return -1;
}

/* Decodifica um caractere UTF-8; devolve bytes consumidos (0 = inválido). */
static int utf8_decode(const uint8_t *s, uint32_t n, uint32_t *cp)
{
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int len = (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!len || (uint32_t)len > n) return 0;
    uint32_t v = s[0] & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = v << 6 | (s[i] & 0x3F);
    }
    *cp = v;
    return len;
}

static void k_MultiByteToWideChar(CPU *c)
{
    uint32_t cp = resolve_cp(re5_arg(c, 0)), src = re5_arg(c, 2), dst = re5_arg(c, 4);
    int32_t srclen = (int32_t)re5_arg(c, 3), dstlen = (int32_t)re5_arg(c, 5);
    c->eax = 0;
    if (!src || dstlen < 0) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); return; }
    uint32_t n = srclen < 0 ? (uint32_t)strlen(re5_gstr(src)) + 1 : (uint32_t)srclen;
    const uint8_t *s = re5_g2h(src);
    uint32_t out = 0;
    for (uint32_t i = 0; i < n;) {
        uint16_t units[2];
        int nu = 1;
        if (cp == CP_UTF8) {
            uint32_t v;
            int used = utf8_decode(s + i, n - i, &v);
            if (!used) { v = 0xFFFD; used = 1; }
            i += (uint32_t)used;
            if (v >= 0x10000) {
                v -= 0x10000;
                units[0] = (uint16_t)(0xD800 + (v >> 10));
                units[1] = (uint16_t)(0xDC00 + (v & 0x3FF));
                nu = 2;
            } else {
                units[0] = (uint16_t)v;
            }
        } else {
            units[0] = sb_to_wide(cp, s[i++]);
        }
        for (int k = 0; k < nu; k++, out++) {
            if (dstlen) {
                if (out >= (uint32_t)dstlen) { re5_set_last_error(K32_ERROR_INSUFFICIENT_BUFFER); return; }
                k32_w16(dst + 2 * out, units[k]);
            }
        }
    }
    c->eax = out;
}

static void k_WideCharToMultiByte(CPU *c)
{
    uint32_t cp = resolve_cp(re5_arg(c, 0)), src = re5_arg(c, 2), dst = re5_arg(c, 4);
    int32_t srclen = (int32_t)re5_arg(c, 3), dstlen = (int32_t)re5_arg(c, 5);
    uint32_t defchar_va = re5_arg(c, 6), used_va = re5_arg(c, 7);
    c->eax = 0;
    if (!src || dstlen < 0) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); return; }
    uint32_t n = srclen < 0 ? k32_wcslen(src) + 1 : (uint32_t)srclen;
    uint8_t defchar = defchar_va ? k32_r8(defchar_va) : '?';
    int used_default = 0;
    uint32_t out = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t buf[4];
        int nb = 0;
        uint32_t w = k32_r16(src + 2 * i);
        if (cp == CP_UTF8) {
            if (w >= 0xD800 && w < 0xDC00 && i + 1 < n) {
                uint32_t lo = k32_r16(src + 2 * (i + 1));
                if (lo >= 0xDC00 && lo < 0xE000) {
                    w = 0x10000 + ((w - 0xD800) << 10) + (lo - 0xDC00);
                    i++;
                }
            }
            if (w < 0x80) buf[nb++] = (uint8_t)w;
            else if (w < 0x800) { buf[nb++] = (uint8_t)(0xC0 | w >> 6); buf[nb++] = (uint8_t)(0x80 | (w & 0x3F)); }
            else if (w < 0x10000) {
                buf[nb++] = (uint8_t)(0xE0 | w >> 12);
                buf[nb++] = (uint8_t)(0x80 | ((w >> 6) & 0x3F));
                buf[nb++] = (uint8_t)(0x80 | (w & 0x3F));
            } else {
                buf[nb++] = (uint8_t)(0xF0 | w >> 18);
                buf[nb++] = (uint8_t)(0x80 | ((w >> 12) & 0x3F));
                buf[nb++] = (uint8_t)(0x80 | ((w >> 6) & 0x3F));
                buf[nb++] = (uint8_t)(0x80 | (w & 0x3F));
            }
        } else {
            int b = wide_to_sb(cp, (uint16_t)w);
            if (b < 0) { b = defchar; used_default = 1; }
            buf[nb++] = (uint8_t)b;
        }
        for (int k = 0; k < nb; k++, out++) {
            if (dstlen) {
                if (out >= (uint32_t)dstlen) { re5_set_last_error(K32_ERROR_INSUFFICIENT_BUFFER); return; }
                k32_w8(dst + out, buf[k]);
            }
        }
    }
    if (used_va) k32_w32(used_va, (uint32_t)used_default);
    c->eax = out;
}

static void k_GetACP(CPU *c) { c->eax = ACP; }
static void k_GetOEMCP(CPU *c) { c->eax = OEMCP; }
static void k_GetUserDefaultLangID(CPU *c) { c->eax = LANG_ENUS; }
static void k_GetSystemDefaultLangID(CPU *c) { c->eax = LANG_ENUS; }

/* CPINFO: MaxCharSize, DefaultChar[2], LeadByte[12]. */
static void k_GetCPInfo(CPU *c)
{
    uint32_t cp = resolve_cp(re5_arg(c, 0)), info = re5_arg(c, 1);
    memset(re5_g2h(info), 0, 18);
    k32_w32(info, cp == CP_UTF8 ? 4 : 1);
    k32_w8(info + 4, '?');
    c->eax = 1;
}

/* ---- classificação de caracteres (CT_CTYPE1) ---- */

#define C1_UPPER   0x0001u
#define C1_LOWER   0x0002u
#define C1_DIGIT   0x0004u
#define C1_SPACE   0x0008u
#define C1_PUNCT   0x0010u
#define C1_CNTRL   0x0020u
#define C1_BLANK   0x0040u
#define C1_XDIGIT  0x0080u
#define C1_ALPHA   0x0100u
#define C1_DEFINED 0x0200u
#define CT_CTYPE1  1u
#define CT_CTYPE2  2u
#define CT_CTYPE3  4u

static int is_upper_w(uint32_t w) { return (w >= 'A' && w <= 'Z') || (w >= 0xC0 && w <= 0xDE && w != 0xD7); }
static int is_lower_w(uint32_t w) { return (w >= 'a' && w <= 'z') || (w >= 0xDF && w <= 0xFF && w != 0xF7); }

static uint16_t ctype1(uint32_t w)
{
    uint16_t f = C1_DEFINED;
    if (w < 0x20 || (w >= 0x7F && w < 0xA0)) f |= C1_CNTRL;
    if (w == ' ' || w == '\t' || w == 0xA0) f |= C1_BLANK;
    if (w == ' ' || (w >= 0x09 && w <= 0x0D) || w == 0xA0) f |= C1_SPACE;
    if (w >= '0' && w <= '9') f |= C1_DIGIT | C1_XDIGIT;
    if ((w >= 'a' && w <= 'f') || (w >= 'A' && w <= 'F')) f |= C1_XDIGIT;
    if (is_upper_w(w)) f |= C1_UPPER | C1_ALPHA;
    else if (is_lower_w(w)) f |= C1_LOWER | C1_ALPHA;
    else if (w >= 0x100) f |= C1_ALPHA;   /* fora do Latin-1: o jogo não classifica */
    if (!(f & (C1_CNTRL | C1_SPACE | C1_DIGIT | C1_ALPHA)) && w > 0x20 && w != 0x7F) f |= C1_PUNCT;
    if (w == 0xA0) f &= (uint16_t)~C1_PUNCT;
    return f;
}

static uint16_t ctype_for(uint32_t type, uint32_t w)
{
    if (type == CT_CTYPE1) return ctype1(w);
    if (type == CT_CTYPE2 || type == CT_CTYPE3) return 0;   /* o CRT só consulta o tipo 1 */
    re5_fatal("kernel32", "GetStringType: tipo %u sem implementação", type);
}

static void k_GetStringTypeW(CPU *c)
{
    uint32_t type = re5_arg(c, 0), src = re5_arg(c, 1), out = re5_arg(c, 3);
    int32_t n = (int32_t)re5_arg(c, 2);
    if (n < 0) n = (int32_t)k32_wcslen(src) + 1;
    for (int32_t i = 0; i < n; i++) k32_w16(out + 2 * (uint32_t)i, ctype_for(type, k32_r16(src + 2 * (uint32_t)i)));
    c->eax = 1;
}

static void k_GetStringTypeA(CPU *c)
{
    uint32_t type = re5_arg(c, 1), src = re5_arg(c, 2), out = re5_arg(c, 4);
    int32_t n = (int32_t)re5_arg(c, 3);
    if (n < 0) n = (int32_t)strlen(re5_gstr(src)) + 1;
    for (int32_t i = 0; i < n; i++)
        k32_w16(out + 2 * (uint32_t)i, ctype_for(type, sb_to_wide(ACP, k32_r8(src + (uint32_t)i))));
    c->eax = 1;
}

/* ---- maiúsculas/minúsculas e comparação ---- */

#define LCMAP_LOWERCASE 0x00000100u
#define LCMAP_UPPERCASE 0x00000200u
#define NORM_IGNORECASE 0x00000001u

static uint32_t to_lower_w(uint32_t w) { return is_upper_w(w) ? w + 0x20 : w; }
static uint32_t to_upper_w(uint32_t w)
{
    if (w == 0xDF || w == 0xFF) return w;   /* ß, ÿ: sem maiúscula de 1 unidade no Latin-1 */
    return is_lower_w(w) ? w - 0x20 : w;
}

static uint32_t map_case(uint32_t flags, uint32_t w)
{
    if (flags & ~(LCMAP_LOWERCASE | LCMAP_UPPERCASE))
        re5_fatal("kernel32", "LCMapString: flags 0x%X sem implementação", flags);
    if (flags & LCMAP_LOWERCASE) return to_lower_w(w);
    if (flags & LCMAP_UPPERCASE) return to_upper_w(w);
    return w;
}

static void k_LCMapStringW(CPU *c)
{
    uint32_t flags = re5_arg(c, 1), src = re5_arg(c, 2), dst = re5_arg(c, 4);
    int32_t n = (int32_t)re5_arg(c, 3), dn = (int32_t)re5_arg(c, 5);
    if (n < 0) n = (int32_t)k32_wcslen(src) + 1;
    c->eax = 0;
    if (dn && dn < n) { re5_set_last_error(K32_ERROR_INSUFFICIENT_BUFFER); return; }
    for (int32_t i = 0; i < n && dn; i++)
        k32_w16(dst + 2 * (uint32_t)i, (uint16_t)map_case(flags, k32_r16(src + 2 * (uint32_t)i)));
    c->eax = (uint32_t)n;
}

static void k_LCMapStringA(CPU *c)
{
    uint32_t flags = re5_arg(c, 1), src = re5_arg(c, 2), dst = re5_arg(c, 4);
    int32_t n = (int32_t)re5_arg(c, 3), dn = (int32_t)re5_arg(c, 5);
    if (n < 0) n = (int32_t)strlen(re5_gstr(src)) + 1;
    c->eax = 0;
    if (dn && dn < n) { re5_set_last_error(K32_ERROR_INSUFFICIENT_BUFFER); return; }
    for (int32_t i = 0; i < n && dn; i++) {
        int b = wide_to_sb(ACP, (uint16_t)map_case(flags, sb_to_wide(ACP, k32_r8(src + (uint32_t)i))));
        k32_w8(dst + (uint32_t)i, (uint8_t)(b < 0 ? '?' : b));
    }
    c->eax = (uint32_t)n;
}

/* CSTR_LESS_THAN 1, CSTR_EQUAL 2, CSTR_GREATER_THAN 3. Ordem por unidade de
 * código (não a ordenação linguística do Windows): basta para igualdade e
 * para o que o jogo ordena (nomes de arquivo ASCII). */
static uint32_t compare(uint32_t flags, const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb)
{
    for (uint32_t i = 0; i < na && i < nb; i++) {
        uint32_t x = a[i], y = b[i];
        if (flags & NORM_IGNORECASE) { x = to_lower_w(x); y = to_lower_w(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;
}

#define CMP_MAX 4096u

static uint32_t load_str(uint32_t va, int32_t n, int wide, uint32_t *out)
{
    uint32_t len = n >= 0 ? (uint32_t)n : wide ? k32_wcslen(va) : (uint32_t)strlen(re5_gstr(va));
    if (len > CMP_MAX) re5_fatal("kernel32", "CompareString: %u caracteres (máximo %u)", len, CMP_MAX);
    for (uint32_t i = 0; i < len; i++) out[i] = wide ? k32_r16(va + 2 * i) : sb_to_wide(ACP, k32_r8(va + i));
    /* Com tamanho explícito, o Windows para no primeiro NUL. */
    for (uint32_t i = 0; i < len; i++) if (!out[i]) return i;
    return len;
}

static void compare_common(CPU *c, int wide)
{
    static uint32_t a[CMP_MAX], b[CMP_MAX];
    uint32_t flags = re5_arg(c, 1);
    if (flags & ~NORM_IGNORECASE) re5_fatal("kernel32", "CompareString: flags 0x%X sem implementação", flags);
    uint32_t na = load_str(re5_arg(c, 2), (int32_t)re5_arg(c, 3), wide, a);
    uint32_t nb = load_str(re5_arg(c, 4), (int32_t)re5_arg(c, 5), wide, b);
    c->eax = compare(flags, a, na, b, nb);
}

static void k_CompareStringA(CPU *c) { compare_common(c, 0); }
static void k_CompareStringW(CPU *c) { compare_common(c, 1); }

/* ---- GetLocaleInfoA ---- */

#define LOCALE_NOUSEROVERRIDE 0x80000000u
#define LOCALE_RETURN_NUMBER  0x20000000u
#define LOCALE_USE_CP_ACP     0x40000000u

static const struct { uint32_t type; const char *value; } k_locale[] = {
    {0x0001, "0409"},           /* LOCALE_ILANGUAGE */
    {0x0002, "English (United States)"},
    {0x0003, "ENU"},            /* LOCALE_SABBREVLANGNAME */
    {0x0004, "English"},        /* LOCALE_SNATIVELANGNAME */
    {0x0005, "1"},              /* LOCALE_ICOUNTRY */
    {0x0006, "United States"},
    {0x0007, "USA"},            /* LOCALE_SABBREVCTRYNAME */
    {0x000B, "437"},            /* LOCALE_IDEFAULTCODEPAGE */
    {0x000C, ","},              /* LOCALE_SLIST */
    {0x000E, "."},              /* LOCALE_SDECIMAL */
    {0x000F, ","},              /* LOCALE_STHOUSAND */
    {0x0010, "3;0"},            /* LOCALE_SGROUPING */
    {0x0011, "2"},              /* LOCALE_IDIGITS */
    {0x0012, "1"},              /* LOCALE_ILZERO */
    {0x0014, "$"},              /* LOCALE_SCURRENCY */
    {0x0015, "USD"},            /* LOCALE_SINTLSYMBOL */
    {0x0016, "."},              /* LOCALE_SMONDECIMALSEP */
    {0x0017, ","},              /* LOCALE_SMONTHOUSANDSEP */
    {0x0018, "3;0"},            /* LOCALE_SMONGROUPING */
    {0x0019, "2"},              /* LOCALE_ICURRDIGITS */
    {0x001B, "0"},              /* LOCALE_ICURRENCY */
    {0x001C, "0"},              /* LOCALE_INEGCURR */
    {0x001D, "/"},              /* LOCALE_SDATE */
    {0x001E, ":"},              /* LOCALE_STIME */
    {0x001F, "M/d/yyyy"},       /* LOCALE_SSHORTDATE */
    {0x0020, "dddd, MMMM d, yyyy"},
    {0x0021, "0"},              /* LOCALE_IDATE */
    {0x0023, "0"},              /* LOCALE_ITIME */
    {0x0028, "AM"},             /* LOCALE_S1159 */
    {0x0029, "PM"},             /* LOCALE_S2359 */
    {0x0050, ""},               /* LOCALE_SPOSITIVESIGN */
    {0x0051, "-"},              /* LOCALE_SNEGATIVESIGN */
    {0x0059, "en"},             /* LOCALE_SISO639LANGNAME */
    {0x005A, "US"},             /* LOCALE_SISO3166CTRYNAME */
    {0x1001, "English"},        /* LOCALE_SENGLANGUAGE */
    {0x1002, "United States"},  /* LOCALE_SENGCOUNTRY */
    {0x1003, "h:mm:ss tt"},     /* LOCALE_STIMEFORMAT */
    {0x1004, "1252"},           /* LOCALE_IDEFAULTANSICODEPAGE */
    {0x1009, "1"},              /* LOCALE_ICALENDARTYPE: gregoriano */
    {0x100C, "6"},              /* LOCALE_IFIRSTDAYOFWEEK: domingo */
};

static void k_GetLocaleInfoA(CPU *c)
{
    uint32_t type = re5_arg(c, 1), buf = re5_arg(c, 2);
    int32_t n = (int32_t)re5_arg(c, 3);
    uint32_t base = type & ~(LOCALE_NOUSEROVERRIDE | LOCALE_RETURN_NUMBER | LOCALE_USE_CP_ACP);
    const char *v = NULL;
    for (size_t i = 0; i < sizeof k_locale / sizeof k_locale[0]; i++)
        if (k_locale[i].type == base) v = k_locale[i].value;
    if (!v) re5_fatal("kernel32", "GetLocaleInfoA(lcid 0x%X, tipo 0x%X) sem implementação", re5_arg(c, 0), type);
    c->eax = 0;
    if (type & LOCALE_RETURN_NUMBER) {
        if (n && n < 4) { re5_set_last_error(K32_ERROR_INSUFFICIENT_BUFFER); return; }
        if (n) k32_w32(buf, (uint32_t)strtoul(v, NULL, base == 0x0001 ? 16 : 10));
        c->eax = 4;   /* em TCHARs da versão A: sizeof(DWORD) */
        return;
    }
    uint32_t len = (uint32_t)strlen(v) + 1;
    if (n) {
        if ((uint32_t)n < len) { re5_set_last_error(K32_ERROR_INSUFFICIENT_BUFFER); return; }
        memcpy(re5_g2h(buf), v, len);
    }
    c->eax = len;
}

/* ---- LocaleNameToLCID (Vista+) ---- */

static const struct { const char *name; uint32_t lcid; } k_locale_names[] = {
    {"en-US", 0x0409}, {"en-GB", 0x0809}, {"ja-JP", 0x0411}, {"fr-FR", 0x040C}, {"de-DE", 0x0407},
    {"it-IT", 0x0410}, {"es-ES", 0x0C0A}, {"pt-BR", 0x0416}, {"pt-PT", 0x0816}, {"ko-KR", 0x0412},
    {"zh-TW", 0x0404}, {"zh-CN", 0x0804}, {"ru-RU", 0x0419}, {"pl-PL", 0x0415}, {"nl-NL", 0x0413},
    {"sv-SE", 0x041D}, {"da-DK", 0x0406}, {"nb-NO", 0x0414}, {"fi-FI", 0x040B}, {"cs-CZ", 0x0405},
    {"hu-HU", 0x040E}, {"tr-TR", 0x041F},
    {"!x-sys-default-locale", LANG_ENUS},   /* LOCALE_NAME_SYSTEM_DEFAULT */
    {"", 0x007F},                            /* LOCALE_NAME_INVARIANT */
};

static void k_LocaleNameToLCID(CPU *c)
{
    uint32_t va = re5_arg(c, 0);
    c->eax = 0;
    if (!va) { c->eax = LANG_ENUS; return; }   /* LOCALE_NAME_USER_DEFAULT */
    char name[86];
    uint32_t n = 0;
    for (uint16_t w; n + 1 < sizeof name && (w = k32_r16(va + 2 * n)) != 0; n++) name[n] = w < 0x80 ? (char)w : '?';
    name[n] = 0;
    for (size_t i = 0; i < sizeof k_locale_names / sizeof k_locale_names[0]; i++) {
        const char *a = k_locale_names[i].name, *b = name;
        while (*a && (*a | 32) == (*b | 32)) a++, b++;
        if (!*a && !*b) { c->eax = k_locale_names[i].lcid; return; }
    }
    RE5_WARN("kernel32", "LocaleNameToLCID(\"%s\"): locale desconhecido", name);
    re5_set_last_error(K32_ERROR_INVALID_PARAMETER);
}

static const re5_import_def k_defs[] = {
    {"CompareStringA",          k_CompareStringA,          24, 0},
    {"CompareStringW",          k_CompareStringW,          24, 0},
    {"GetACP",                  k_GetACP,                   0, 0},
    {"GetCPInfo",               k_GetCPInfo,                8, 0},
    {"GetLocaleInfoA",          k_GetLocaleInfoA,          16, 0},
    {"GetOEMCP",                k_GetOEMCP,                 0, 0},
    {"GetStringTypeA",          k_GetStringTypeA,          20, 0},
    {"GetStringTypeW",          k_GetStringTypeW,          16, 0},
    {"GetSystemDefaultLangID",  k_GetSystemDefaultLangID,   0, 0},
    {"GetUserDefaultLangID",    k_GetUserDefaultLangID,     0, 0},
    {"LCMapStringA",            k_LCMapStringA,            24, 0},
    {"LCMapStringW",            k_LCMapStringW,            24, 0},
    {"LocaleNameToLCID",        k_LocaleNameToLCID,         8, 0},
    {"MultiByteToWideChar",     k_MultiByteToWideChar,     24, 0},
    {"WideCharToMultiByte",     k_WideCharToMultiByte,     32, 0},
    {0},
};

const re5_import_module re5_mod_kernel32_nls = {"kernel32.dll", k_defs};
