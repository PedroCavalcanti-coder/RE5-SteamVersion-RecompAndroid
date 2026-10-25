/*
 * lan_menu.c - menu de perfil do multiplayer LAN por cima do quadro (ver re5/lan_menu.h).
 *
 * O jogo não tem tela para o nome nem para a bandeira (vinham da conta Steam), então
 * o runtime desenha a sua, como o coldebug: triângulos coloridos no back buffer do
 * host, antes do Present, com o estado do device salvo e restaurado. O texto é uma
 * fonte bitmap 8x16 (lan_font.h, gerada por scripts/gen_lan_font.py) em que cada
 * glifo vira poucos retângulos; nada de textura nem shader.
 *
 * O layout é calculado na mesma passada que desenha: as áreas de toque ficam em
 * coordenadas normalizadas (0..1 da tela) e servem para o toque (SDL_FINGER*, que já
 * vem normalizado) e para o mouse (dividido pelo tamanho da janela).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
#endif
#include <d3d9.h>

#include "re5/lan_menu.h"
#include "re5/log.h"
#include "re5/steam_lan.h"
#include "lan_font.h"

/* Bandeiras oferecidas no ciclo de "<" / ">". Qualquer outro código de duas letras
 * pode ser digitado no campo do país. */
typedef struct { const char *code, *name; } country;
static const country k_countries[] = {
    {"BR", "Brasil"}, {"PT", "Portugal"}, {"US", "Estados Unidos"}, {"AO", "Angola"}, {"MZ", "Moçambique"},
    {"CV", "Cabo Verde"}, {"AR", "Argentina"}, {"BO", "Bolívia"}, {"CL", "Chile"}, {"CO", "Colômbia"},
    {"EC", "Equador"}, {"MX", "México"}, {"PY", "Paraguai"}, {"PE", "Peru"}, {"UY", "Uruguai"},
    {"VE", "Venezuela"}, {"CA", "Canadá"}, {"GB", "Reino Unido"}, {"IE", "Irlanda"}, {"ES", "Espanha"},
    {"FR", "França"}, {"DE", "Alemanha"}, {"IT", "Itália"}, {"NL", "Holanda"}, {"BE", "Bélgica"},
    {"CH", "Suíça"}, {"AT", "Áustria"}, {"SE", "Suécia"}, {"NO", "Noruega"}, {"DK", "Dinamarca"},
    {"FI", "Finlândia"}, {"PL", "Polônia"}, {"CZ", "Tchéquia"}, {"RU", "Rússia"}, {"UA", "Ucrânia"},
    {"TR", "Turquia"}, {"GR", "Grécia"}, {"IL", "Israel"}, {"SA", "Arábia Saudita"}, {"AE", "Emirados Árabes"},
    {"EG", "Egito"}, {"MA", "Marrocos"}, {"NG", "Nigéria"}, {"ZA", "África do Sul"}, {"IN", "Índia"},
    {"CN", "China"}, {"HK", "Hong Kong"}, {"TW", "Taiwan"}, {"JP", "Japão"}, {"KR", "Coreia do Sul"},
    {"PH", "Filipinas"}, {"ID", "Indonésia"}, {"MY", "Malásia"}, {"SG", "Singapura"}, {"TH", "Tailândia"},
    {"VN", "Vietnã"}, {"AU", "Austrália"}, {"NZ", "Nova Zelândia"},
};
#define N_COUNTRIES ((int)(sizeof k_countries / sizeof k_countries[0]))

enum { F_NAME, F_COUNTRY, F_HOSTS, F_SAVE, F_CLOSE, F_COUNT };
enum { HIT_NAME, HIT_COUNTRY, HIT_PREV, HIT_NEXT, HIT_HOSTS, HIT_SAVE, HIT_CLOSE, HIT_COUNT };

#define HOSTS_MAX 63

#define C_PANEL   0xE0101418u
#define C_BORDER  0xFF8A6A2Au
#define C_TITLE   0xFFF2C14Eu
#define C_TEXT    0xFFE8E8E8u
#define C_DIM     0xFF9AA0A6u
#define C_FIELD   0xFF262C33u
#define C_FOCUS   0xFF3B4A5Cu
#define C_BUTTON  0xFF5A4520u
#define C_OK      0xFF6CD36Cu
#define C_WARN    0xFFF0C040u
#define C_BAD     0xFFF06060u

static SDL_mutex *g_mu;
static int g_open, g_focus;
static char g_name[RE5_LAN_MAX_NAME_LEN + 1], g_cc[4], g_cc_typed[3], g_hosts[HOSTS_MAX + 1];
static char g_msg[96];
static uint32_t g_msg_color;
static float g_hit[HIT_COUNT][4];   /* x0, y0, x1, y1 normalizados */
static int g_have_hits;
static int g_fingers, g_gesture;

static void lock(void)
{
    if (!g_mu) g_mu = SDL_CreateMutex();
    SDL_LockMutex(g_mu);
}

static void unlock(void) { SDL_UnlockMutex(g_mu); }

static int country_index(const char *cc)
{
    for (int i = 0; i < N_COUNTRIES; i++)
        if (strcmp(k_countries[i].code, cc) == 0) return i;
    return -1;
}

static const char *country_name(const char *cc)
{
    int i = country_index(cc);
    return i >= 0 ? k_countries[i].name : "outro";
}

static void cycle_country(int d)
{
    int i = country_index(g_cc);
    i = i < 0 ? (d > 0 ? 0 : N_COUNTRIES - 1) : (i + d + N_COUNTRIES) % N_COUNTRIES;
    snprintf(g_cc, sizeof g_cc, "%s", k_countries[i].code);
    g_cc_typed[0] = 0;
}

static int is_text_field(int f) { return f == F_NAME || f == F_HOSTS || f == F_COUNTRY; }

/* No Android o teclado virtual só aparece com a entrada de texto ligada. */
static void text_input(int on)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
    if (on) SDL_StartTextInput();
#if defined(__ANDROID__)
    else SDL_StopTextInput();
#endif
}

static void set_focus(int f)
{
    g_focus = (f + F_COUNT) % F_COUNT;
    g_cc_typed[0] = 0;
    text_input(is_text_field(g_focus));
}

static void open_menu(void)
{
    snprintf(g_name, sizeof g_name, "%s", steam_lan_get_local_name());
    steam_lan_get_local_country(g_cc);
    steam_lan_get_manual_hosts(g_hosts, sizeof g_hosts);
    g_msg[0] = 0;
    g_open = 1;
    set_focus(F_NAME);
    RE5_INFO("lan_menu", "menu LAN aberto");
}

static void close_menu(void)
{
    g_open = 0;
    text_input(0);
}

static void save(void)
{
    int n = steam_lan_set_manual_hosts(g_hosts);
    if (g_hosts[0] && n == 0) {
        snprintf(g_msg, sizeof g_msg, "IP do host inválido (ex.: 192.168.0.10)");
        g_msg_color = C_BAD;
        set_focus(F_HOSTS);
        return;
    }
    if (!steam_lan_set_profile(g_name, g_cc)) {
        snprintf(g_msg, sizeof g_msg, g_name[0] ? "País inválido (duas letras, ex.: BR)" : "O nome não pode ficar vazio");
        g_msg_color = C_BAD;
        set_focus(g_name[0] ? F_COUNTRY : F_NAME);
        return;
    }
    close_menu();
}

static void type_text(const char *t)
{
    for (; *t; t++) {
        unsigned char ch = (unsigned char)*t;
        if (g_focus == F_NAME) {
            size_t n = strlen(g_name);
            if (ch >= 0x20 && ch < 0x7F && ch != '#' && n < RE5_LAN_MAX_NAME_LEN) {
                g_name[n] = (char)ch;
                g_name[n + 1] = 0;
            }
        } else if (g_focus == F_HOSTS) {
            size_t n = strlen(g_hosts);
            if (((ch >= '0' && ch <= '9') || ch == '.' || ch == ',') && n < HOSTS_MAX) {
                g_hosts[n] = (char)ch;
                g_hosts[n + 1] = 0;
            }
        } else if (g_focus == F_COUNTRY) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) continue;
            size_t n = strlen(g_cc_typed);
            g_cc_typed[n] = (char)(ch & ~0x20);
            g_cc_typed[n + 1] = 0;
            if (n == 0) {   /* 1ª letra: o primeiro país da lista com ela */
                for (int i = 0; i < N_COUNTRIES; i++)
                    if (k_countries[i].code[0] == g_cc_typed[0]) {
                        snprintf(g_cc, sizeof g_cc, "%s", k_countries[i].code);
                        break;
                    }
            } else {        /* 2ª letra: o código digitado, esteja na lista ou não */
                snprintf(g_cc, sizeof g_cc, "%s", g_cc_typed);
                g_cc_typed[0] = 0;
            }
        }
    }
}

static void backspace(void)
{
    char *s = g_focus == F_NAME ? g_name : g_focus == F_HOSTS ? g_hosts : NULL;
    if (s && *s) s[strlen(s) - 1] = 0;
    if (g_focus == F_COUNTRY) g_cc_typed[0] = 0;
}

static void on_key(const SDL_KeyboardEvent *k)
{
    int shift = (k->keysym.mod & KMOD_SHIFT) != 0;
    switch (k->keysym.scancode) {
    case SDL_SCANCODE_ESCAPE: close_menu(); break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        if (g_focus == F_CLOSE) close_menu();
        else save();
        break;
    case SDL_SCANCODE_TAB: set_focus(g_focus + (shift ? -1 : 1)); break;
    case SDL_SCANCODE_DOWN: set_focus(g_focus + 1); break;
    case SDL_SCANCODE_UP: set_focus(g_focus - 1); break;
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT: {
        int d = k->keysym.scancode == SDL_SCANCODE_RIGHT ? 1 : -1;
        if (g_focus == F_COUNTRY) cycle_country(d);
        else if (g_focus == F_SAVE || g_focus == F_CLOSE) set_focus(g_focus == F_SAVE ? F_CLOSE : F_SAVE);
        break;
    }
    case SDL_SCANCODE_BACKSPACE: backspace(); break;
    default: break;
    }
}

static int hit_at(float x, float y)
{
    for (int i = 0; i < HIT_COUNT; i++)
        if (x >= g_hit[i][0] && x < g_hit[i][2] && y >= g_hit[i][1] && y < g_hit[i][3]) return i;
    return -1;
}

static void tap(float x, float y)
{
    if (!g_have_hits) re5_lan_menu_build(1280.0f, 720.0f, NULL, 0);   /* ainda não desenhou: layout padrão */
    switch (hit_at(x, y)) {
    case HIT_NAME: set_focus(F_NAME); break;
    case HIT_COUNTRY: set_focus(F_COUNTRY); break;
    case HIT_PREV: set_focus(F_COUNTRY); cycle_country(-1); break;
    case HIT_NEXT: set_focus(F_COUNTRY); cycle_country(1); break;
    case HIT_HOSTS: set_focus(F_HOSTS); break;
    case HIT_SAVE: save(); break;
    case HIT_CLOSE: close_menu(); break;
    default: break;
    }
}

static void toggle_locked(void)
{
    if (g_open) close_menu();
    else open_menu();
}

void re5_lan_menu_toggle(void)
{
    lock();
    toggle_locked();
    unlock();
}

int re5_lan_menu_is_open(void)
{
    lock();
    int o = g_open;
    unlock();
    return o;
}

int re5_lan_menu_event(const SDL_Event *e)
{
    int used = 0;
    lock();
    switch (e->type) {
    case SDL_KEYDOWN:
        if (e->key.keysym.scancode == SDL_SCANCODE_F12) {
            if (!e->key.repeat) toggle_locked();
            used = 1;
        } else if (g_open) {
            on_key(&e->key);
            used = 1;
        }
        break;
    case SDL_KEYUP:
        used = e->key.keysym.scancode == SDL_SCANCODE_F12;   /* as outras soltam a tecla no jogo também */
        break;
    case SDL_TEXTINPUT:
        if (g_open) {
            type_text(e->text.text);
            used = 1;
        }
        break;
    case SDL_FINGERDOWN:
        g_fingers++;
        if (g_fingers >= 3 && !g_gesture) {
            g_gesture = 1;   /* o resto deste toque é do gesto */
            toggle_locked();
        }
        if (g_gesture) used = 1;
        else if (g_open) {
            tap(e->tfinger.x, e->tfinger.y);
            used = 1;
        }
        break;
    case SDL_FINGERUP:
        if (g_fingers > 0) g_fingers--;
        used = g_gesture || g_open;
        if (!g_fingers) g_gesture = 0;
        break;
    case SDL_FINGERMOTION: used = g_gesture || g_open; break;
    case SDL_MOUSEBUTTONDOWN:
        if (!g_open) break;
        used = 1;
        if (e->button.which != SDL_TOUCH_MOUSEID && e->button.button == SDL_BUTTON_LEFT) {   /* toque já veio como FINGER */
            SDL_Window *w = SDL_GetWindowFromID(e->button.windowID);
            int ww = 0, wh = 0;
            if (w) SDL_GetWindowSize(w, &ww, &wh);
            if (ww > 0 && wh > 0) tap((float)e->button.x / (float)ww, (float)e->button.y / (float)wh);
        }
        break;
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEWHEEL: used = g_open; break;
    default: break;
    }
    unlock();
    return used;
}

/* ---- desenho ---- */

#define N_GLYPHS 191
#define MAX_GRECTS 40
static uint8_t g_grect[N_GLYPHS][MAX_GRECTS][4];   /* x, y, w, h em pixels da fonte */
static uint8_t g_ngrect[N_GLYPHS];
static int g_grects_ready;

/* Cada glifo em retângulos: trechos acesos de uma linha, emendados com os
 * trechos iguais da linha de baixo. */
static void build_glyph_rects(void)
{
    for (int g = 0; g < N_GLYPHS; g++) {
        int n = 0;
        uint8_t used[LAN_FONT_H] = {0};
        for (int y = 0; y < LAN_FONT_H; y++) {
            uint8_t row = lan_font[g][y] & (uint8_t)~used[y];
            for (int x = 0; x < LAN_FONT_W;) {
                if (!(row & (0x80 >> x))) {
                    x++;
                    continue;
                }
                int x1 = x;
                while (x1 < LAN_FONT_W && (row & (0x80 >> x1))) x1++;
                uint8_t mask = 0;
                for (int k = x; k < x1; k++) mask |= (uint8_t)(0x80 >> k);
                int y1 = y + 1;
                /* desce enquanto a linha tem exatamente o mesmo trecho (sem vizinhos acesos) */
                while (y1 < LAN_FONT_H && (lan_font[g][y1] & mask) == mask &&
                       !(lan_font[g][y1] & (x > 0 ? (0x80 >> (x - 1)) : 0)) &&
                       !(lan_font[g][y1] & (x1 < LAN_FONT_W ? (0x80 >> x1) : 0)) && !(used[y1] & mask))
                    used[y1++] |= mask;
                if (n < MAX_GRECTS) {
                    g_grect[g][n][0] = (uint8_t)x;
                    g_grect[g][n][1] = (uint8_t)y;
                    g_grect[g][n][2] = (uint8_t)(x1 - x);
                    g_grect[g][n][3] = (uint8_t)(y1 - y);
                    n++;
                }
                x = x1;
            }
        }
        g_ngrect[g] = (uint8_t)n;
    }
    g_grects_ready = 1;
}

static re5_lan_vtx *g_out;
static uint32_t g_nv, g_maxv;
static float g_w, g_h, g_clip;   /* g_clip: texto não passa da borda direita do painel */

static void quad(float x0, float y0, float x1, float y1, uint32_t color)
{
    if (!g_out) {
        g_nv += 6;
        return;
    }
    if (g_nv + 6 > g_maxv) return;
    const float px[6] = {x0, x1, x0, x1, x1, x0}, py[6] = {y0, y0, y1, y0, y1, y1};
    for (int i = 0; i < 6; i++) g_out[g_nv++] = (re5_lan_vtx){px[i] - 0.5f, py[i] - 0.5f, 0.0f, 1.0f, color};
}

/* UTF-8 -> índice do glifo (ASCII e Latin-1; o resto vira '?'). */
static int next_glyph(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp = '?';
    if (p[0] < 0x80) {
        cp = p[0];
        *s += 1;
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *s += 2;
    } else {
        *s += 1;
        while ((**s & 0xC0) == 0x80) *s += 1;
    }
    if (cp >= 0x20 && cp < 0x7F) return (int)cp - 0x20;
    if (cp >= 0xA0 && cp <= 0xFF) return (int)cp - 0xA0 + 95;
    return '?' - 0x20;
}

static int text_len(const char *s)
{
    int n = 0;
    while (*s) {
        next_glyph(&s);
        n++;
    }
    return n;
}

static float text(float x, float y, float sc, const char *s, uint32_t color)
{
    while (*s) {
        int g = next_glyph(&s);
        if (x + LAN_FONT_W * sc > g_clip) break;
        for (int r = 0; r < g_ngrect[g]; r++) {
            const uint8_t *q = g_grect[g][r];
            quad(x + q[0] * sc, y + q[1] * sc, x + (q[0] + q[2]) * sc, y + (q[1] + q[3]) * sc, color);
        }
        x += LAN_FONT_W * sc;
    }
    return x;
}

static void set_hit(int id, float x0, float y0, float x1, float y1)
{
    g_hit[id][0] = x0 / g_w;
    g_hit[id][1] = y0 / g_h;
    g_hit[id][2] = x1 / g_w;
    g_hit[id][3] = y1 / g_h;
}

static void member_line(char *out, size_t cap, uint32_t *color, const re5_lan_member_info *m)
{
    char tail[48];
    *color = C_TEXT;
    if (m->is_local) {
        snprintf(tail, sizeof tail, "você");
    } else if (m->silent_ms > 3000) {
        snprintf(tail, sizeof tail, "sem resposta há %u s", m->silent_ms / 1000u);
        *color = C_BAD;
    } else if (m->rtt_ms < 0) {
        snprintf(tail, sizeof tail, "medindo ping...");
    } else {
        snprintf(tail, sizeof tail, "ping %d ms", (int)m->rtt_ms);
        *color = m->rtt_ms < 60 ? C_OK : m->rtt_ms < 150 ? C_WARN : C_BAD;
    }
    snprintf(out, cap, "%s #%u [%s]%s  -  %s", m->name, (uint32_t)m->steam_id, m->country[0] ? m->country : "--",
             m->is_owner ? " (host)" : "", tail);
}

uint32_t re5_lan_menu_build(float w, float h, re5_lan_vtx *out, uint32_t max)
{
    if (!g_grects_ready) build_glyph_rects();
    g_out = out;
    g_nv = 0;
    g_maxv = max;
    g_w = w;
    g_h = h;

    /* Escala inteira (pixels nítidos): ~16 linhas de texto cabem em 90% da altura. */
    const int cols = 74;
    int s = (int)(h / 300.0f);
    if (s < 1) s = 1;
    while (s > 1 && cols * LAN_FONT_W * s > w * 0.96f) s--;
    const float sc = (float)s, cw = LAN_FONT_W * sc, lh = 20.0f * sc, pad = 10.0f * sc;
    const float pw = cols * cw + 2 * pad, ph = 16 * lh + 2 * pad;
    const float px = (w - pw) * 0.5f, py = (h - ph) * 0.5f;
    const float tx = px + pad, fx = tx + 14 * cw, fw = pw - pad - (fx - px);
    float y = py + pad;
    g_clip = px + pw - pad;

    quad(px - 2 * sc, py - 2 * sc, px + pw + 2 * sc, py + ph + 2 * sc, C_BORDER);
    quad(px, py, px + pw, py + ph, C_PANEL);

    text(tx, y, sc, "Multiplayer LAN - seu perfil", C_TITLE);
    y += lh;
    re5_lan_status st;
    steam_lan_get_status(&st);
    char line[160];
    snprintf(line, sizeof line, "Seu ID: #%u    IP deste aparelho: %s (porta %u)", (uint32_t)st.local_id, st.local_ip,
             st.p2p_port);
    text(tx, y, sc, line, C_DIM);
    y += lh * 1.5f;

    /* Campos */
    const char *labels[3] = {"Nome:", "País:", "IP do host:"};
    for (int f = 0; f < 3; f++) {
        float y0 = y - 3 * sc, y1 = y + LAN_FONT_H * sc + 3 * sc;
        int focused = g_focus == f;
        text(tx, y, sc, labels[f], focused ? C_TITLE : C_TEXT);
        quad(fx - 4 * sc, y0, fx + fw, y1, focused ? C_FOCUS : C_FIELD);
        int caret = focused && (SDL_GetTicks() / 500u) % 2u == 0;
        if (f == F_NAME) {
            float e = text(fx, y, sc, g_name, C_TEXT);
            if (caret) quad(e, y, e + 2 * sc, y + LAN_FONT_H * sc, C_TEXT);
            set_hit(HIT_NAME, px, y0, px + pw, y1);
        } else if (f == F_COUNTRY) {
            float bx0 = fx - 4 * sc, bx1 = fx + 3 * cw;
            quad(bx0, y0, bx1, y1, C_BUTTON);
            text(fx + cw * 0.5f, y, sc, "<", C_TEXT);
            snprintf(line, sizeof line, "%s  %s", g_cc, country_name(g_cc));
            float e = text(fx + 4 * cw, y, sc, line, C_TEXT);
            if (focused && g_cc_typed[0]) text(e + cw, y, sc, g_cc_typed, C_WARN);
            float nx0 = fx + fw - 3 * cw;
            quad(nx0, y0, fx + fw, y1, C_BUTTON);
            text(nx0 + cw, y, sc, ">", C_TEXT);
            set_hit(HIT_PREV, bx0, y0, bx1, y1);
            set_hit(HIT_NEXT, nx0, y0, fx + fw, y1);
            set_hit(HIT_COUNTRY, px, y0, bx0, y1);
        } else {
            float e = text(fx, y, sc, g_hosts[0] || focused ? g_hosts : "(vazio: procurar na rede)",
                           g_hosts[0] || focused ? C_TEXT : C_DIM);
            if (caret) quad(e, y, e + 2 * sc, y + LAN_FONT_H * sc, C_TEXT);
            set_hit(HIT_HOSTS, px, y0, px + pw, y1);
        }
        y += lh * 1.25f;
    }
    text(fx, y, sc, "Bandeira e nome valem na próxima sala.", C_DIM);
    y += lh * 1.5f;

    /* Sala */
    if (st.in_lobby && st.is_host) snprintf(line, sizeof line, "Hospedando a sala (%d/%d jogadores)", st.member_count, st.max_members);
    else if (st.in_lobby) snprintf(line, sizeof line, "Na sala de outro jogador (%d/%d)", st.member_count, st.max_members);
    else if (st.joining) snprintf(line, sizeof line, "Entrando na sala...");
    else snprintf(line, sizeof line, "Fora de sala - %d sala(s) vista(s) na rede", st.lobbies_seen);
    text(tx, y, sc, line, C_TEXT);
    y += lh;
    for (int i = 0; i < st.member_count; i++) {
        uint32_t color;
        member_line(line, sizeof line, &color, &st.members[i]);
        text(tx + 2 * cw, y, sc, line, color);
        y += lh;
    }
    y = py + pad + 12 * lh;

    /* Botões */
    const char *btn[2] = {"Salvar", "Fechar"};
    float bx = tx;
    for (int b = 0; b < 2; b++) {
        float bw = (float)(text_len(btn[b]) + 4) * cw, y0 = y - 4 * sc, y1 = y + LAN_FONT_H * sc + 4 * sc;
        int focused = g_focus == F_SAVE + b;
        quad(bx, y0, bx + bw, y1, focused ? C_TITLE : C_BUTTON);
        text(bx + 2 * cw, y, sc, btn[b], focused ? 0xFF101010u : C_TEXT);
        set_hit(b ? HIT_CLOSE : HIT_SAVE, bx, y0, bx + bw, y1);
        bx += bw + 2 * cw;
    }
    if (g_msg[0]) text(bx + cw, y, sc, g_msg, g_msg_color);
    y += lh * 1.5f;
    text(tx, y, sc, "Teclado: Tab/setas trocam o campo, < > o país, Enter salva, Esc fecha.", C_DIM);
    y += lh;
    text(tx, y, sc, "Toque: escolha o campo ou o botão. F12 ou três dedos abrem e fecham.", C_DIM);

    g_have_hits = 1;
    g_out = NULL;
    return out ? g_nv : 0;
}

#define MAX_VTX 240000u

void re5_lan_menu_draw(IDirect3DDevice9 *dev)
{
    if (!dev) return;
    lock();
    if (!g_open) {
        unlock();
        return;
    }
    static re5_lan_vtx *vtx;
    if (!vtx) vtx = malloc(sizeof *vtx * MAX_VTX);
    IDirect3DSurface9 *bb = NULL, *rt0 = NULL, *ds = NULL;
    D3DSURFACE_DESC desc;
    if (!vtx || FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        unlock();
        return;
    }
    bb->lpVtbl->GetDesc(bb, &desc);
    uint32_t nv = re5_lan_menu_build((float)desc.Width, (float)desc.Height, vtx, MAX_VTX);
    unlock();

    IDirect3DStateBlock9 *sb = NULL;
    dev->lpVtbl->GetRenderTarget(dev, 0, &rt0);
    dev->lpVtbl->GetDepthStencilSurface(dev, &ds);
    if (nv >= 3 && SUCCEEDED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &sb)) && sb) {
        D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
        dev->lpVtbl->SetRenderTarget(dev, 0, bb);
        dev->lpVtbl->SetDepthStencilSurface(dev, NULL);
        dev->lpVtbl->SetViewport(dev, &vp);
        dev->lpVtbl->SetVertexShader(dev, NULL);
        dev->lpVtbl->SetPixelShader(dev, NULL);
        dev->lpVtbl->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTexture(dev, 1, NULL);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
        dev->lpVtbl->SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        dev->lpVtbl->SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        for (uint32_t off = 0; off + 3 <= nv;) {
            uint32_t n = nv - off;
            if (n > 60000u) n = 60000u;
            dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, n / 3, vtx + off, sizeof *vtx);
            off += n;
        }
        sb->lpVtbl->Apply(sb);
        dev->lpVtbl->SetRenderTarget(dev, 0, rt0);
        dev->lpVtbl->SetDepthStencilSurface(dev, ds);
        sb->lpVtbl->Release(sb);
    }
    if (rt0) rt0->lpVtbl->Release(rt0);
    if (ds) ds->lpVtbl->Release(ds);
    bb->lpVtbl->Release(bb);
}
