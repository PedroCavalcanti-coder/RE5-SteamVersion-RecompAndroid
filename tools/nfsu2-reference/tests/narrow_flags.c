/* Regression for MSVC's optimisation of left-aligned narrow guest flags.
 * Standalone, no game data. Build with /Od, /O1 and /O2 and the recomp32 include. */
#include "recomp_types.h"
#include <stdio.h>

#define TEST_FN(name, cond) static __declspec(noinline) int name(uint16_t v) { \
    uint32_t a = (uint32_t)v << 16, b = (uint32_t)v << 16; return cond(a,b); }
/* Separate consumers: packing flags into one result masks the optimizer bug. */
TEST_FN(sign16, TEST_S)
TEST_FN(nsign16, TEST_NS)
TEST_FN(greater16, TEST_G)
TEST_FN(le16, TEST_LE)
#define CMP_FN(name, cond) static __declspec(noinline) int name(uint16_t x, uint16_t y) { \
    uint32_t a = (uint32_t)x << 16, b = (uint32_t)y << 16; return cond(a,b); }
CMP_FN(less16, CMP_L)
CMP_FN(cle16, CMP_LE)
CMP_FN(cgreater16, CMP_G)
CMP_FN(ge16, CMP_GE)
int main(void) {
    const uint16_t edge[] = {0,1,0x7fff,0x8000,0x8001,0xfffe,0xffff};
    unsigned failures = 0;
    for (unsigned i = 0; i < 65536; i++) {
        int v = (i & 0x8000) ? (int)i - 65536 : (int)i;
        unsigned want = (v < 0) | ((v >= 0) << 1) | ((v > 0) << 2) | ((v <= 0) << 3);
        unsigned got = sign16((uint16_t)i) | (nsign16((uint16_t)i) << 1) |
                       (greater16((uint16_t)i) << 2) | (le16((uint16_t)i) << 3);
        if (got != want) failures++;
        for (unsigned k = 0; k < sizeof edge / sizeof edge[0]; k++) {
            unsigned j = edge[k];
            int w = (j & 0x8000) ? (int)j - 65536 : (int)j;
            want = (v < w) | ((v <= w) << 1) | ((v > w) << 2) | ((v >= w) << 3);
            got = less16((uint16_t)i, (uint16_t)j) | (cle16((uint16_t)i, (uint16_t)j) << 1) |
                  (cgreater16((uint16_t)i, (uint16_t)j) << 2) | (ge16((uint16_t)i, (uint16_t)j) << 3);
            if (got != want) failures++;
        }
    }
    printf("narrow flags: %u failures (65536 tests, 458752 comparisons)\n", failures);
    return failures != 0;
}
