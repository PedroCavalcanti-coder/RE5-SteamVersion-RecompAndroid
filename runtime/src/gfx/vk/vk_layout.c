/*
 * vk_layout.c - ver vk_layout.h.
 */
#include "vk_layout.h"

int re5vk_layout_general_choice(const char *env, int android)
{
    (void)android;
    if (env && *env) return *env == '1';
    return 0;
}

static int skipped(const re5vk_subref *skip, uint32_t nskip, uint32_t layer, uint32_t level)
{
    for (uint32_t i = 0; i < nskip; i++)
        if (skip[i].layer == layer && skip[i].level == level) return 1;
    return 0;
}

uint32_t re5vk_layout_pending(const VkImageLayout *layouts, const uint8_t *dirty, uint32_t levels,
                              uint32_t layers, VkImageLayout want, const re5vk_subref *skip, uint32_t nskip,
                              uint32_t *out, uint32_t cap)
{
    if (!layouts || !dirty) return 0;
    uint32_t n = 0;
    for (uint32_t ly = 0; ly < layers; ly++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            uint32_t i = ly * levels + lv;
            if (layouts[i] == want && !dirty[i]) continue;
            if (skipped(skip, nskip, ly, lv)) continue;
            if (!out) return 1;
            if (n < cap) out[n] = i;
            n++;
        }
    }
    return n;
}
