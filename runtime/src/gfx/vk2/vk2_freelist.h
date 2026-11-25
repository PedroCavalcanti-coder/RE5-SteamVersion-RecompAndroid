/*
 * vk2_freelist.h - lista livre ordenada de um bloco de memória (sub-alocador do vk2_ctx.c), sem Vulkan:
 * por isso tem teste de unidade (tests/unit/test_vk2_freelist.c). Primeiro que serve; ao devolver,
 * junta com os vizinhos livres.
 */
#ifndef VK2_FREELIST_H
#define VK2_FREELIST_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t off, size; } vk2_span;

typedef struct {
    vk2_span *s;
    uint32_t n, cap;
} vk2_fl;

static inline int vk2_fl_init(vk2_fl *f, uint64_t size)
{
    f->s = malloc(16 * sizeof *f->s);
    f->cap = 16;
    f->n = 1;
    if (!f->s) return -1;
    f->s[0] = (vk2_span){0, size};
    return 0;
}

static inline void vk2_fl_destroy(vk2_fl *f)
{
    free(f->s);
    memset(f, 0, sizeof *f);
}

static inline int vk2_fl_grow(vk2_fl *f)
{
    if (f->n < f->cap) return 0;
    vk2_span *n = realloc(f->s, (size_t)f->cap * 2 * sizeof *n);
    if (!n) return -1;
    f->s = n;
    f->cap *= 2;
    return 0;
}

static inline uint64_t vk2_fl_align(uint64_t v, uint64_t a) { return a > 1 ? (v + a - 1) / a * a : v; }

/* 1 = alocou (*off alinhado a `align`); 0 = não coube. */
static inline int vk2_fl_take(vk2_fl *f, uint64_t size, uint64_t align, uint64_t *off)
{
    for (uint32_t i = 0; i < f->n; i++) {
        uint64_t s = f->s[i].off, e = s + f->s[i].size, a = vk2_fl_align(s, align);
        if (a + size > e) continue;
        uint64_t head = a - s, tail = e - (a + size);
        if (head && tail) {   /* parte em dois */
            if (vk2_fl_grow(f)) return 0;
            memmove(&f->s[i + 1], &f->s[i], (f->n - i) * sizeof *f->s);
            f->n++;
            f->s[i] = (vk2_span){s, head};
            f->s[i + 1] = (vk2_span){a + size, tail};
        } else if (head) {
            f->s[i].size = head;
        } else if (tail) {
            f->s[i] = (vk2_span){a + size, tail};
        } else {
            memmove(&f->s[i], &f->s[i + 1], (f->n - i - 1) * sizeof *f->s);
            f->n--;
        }
        *off = a;
        return 1;
    }
    return 0;
}

/* Devolve [off, off+size) e junta com os vizinhos. */
static inline int vk2_fl_give(vk2_fl *f, uint64_t off, uint64_t size)
{
    uint32_t i = 0;
    while (i < f->n && f->s[i].off < off) i++;
    int prev = i && f->s[i - 1].off + f->s[i - 1].size == off;
    int next = i < f->n && off + size == f->s[i].off;
    if (prev && next) {
        f->s[i - 1].size += size + f->s[i].size;
        memmove(&f->s[i], &f->s[i + 1], (f->n - i - 1) * sizeof *f->s);
        f->n--;
    } else if (prev) {
        f->s[i - 1].size += size;
    } else if (next) {
        f->s[i].off = off;
        f->s[i].size += size;
    } else {
        if (vk2_fl_grow(f)) return -1;
        memmove(&f->s[i + 1], &f->s[i], (f->n - i) * sizeof *f->s);
        f->n++;
        f->s[i] = (vk2_span){off, size};
    }
    return 0;
}

static inline uint64_t vk2_fl_free_bytes(const vk2_fl *f)
{
    uint64_t t = 0;
    for (uint32_t i = 0; i < f->n; i++) t += f->s[i].size;
    return t;
}

#endif /* VK2_FREELIST_H */
