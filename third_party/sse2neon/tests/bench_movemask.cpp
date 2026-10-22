/**
 * Benchmark for _mm_movemask_epi8 and related movemask intrinsics.
 *
 * Measures three dimensions:
 *   1. Throughput: independent calls (pipeline utilization)
 *   2. Latency: dependent chain (true instruction latency)
 *   3. In-context: memchr-like string search (realistic usage)
 *
 * Build and run:
 *   make bench-movemask                                    # native x86
 *   make bench-movemask CROSS_COMPILE=arm-linux-gnueabihf- # ARMv7 + QEMU
 *   make bench-movemask CROSS_COMPILE=aarch64-linux-gnu-   # AArch64 + QEMU
 *
 * Inspired by the methodology in PR #704:
 *   https://github.com/DLTcollab/sse2neon/pull/704
 */

#include <benchmark/benchmark.h>

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)
#include "sse2neon.h"
#else
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

#include <cstdint>
#include <cstring>

/* Simple xorshift32 PRNG for reproducible random data. */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

const int N_DATA = 1024;
__m128i data_zero[N_DATA];
__m128i data_all[N_DATA];
__m128i data_alt[N_DATA];
__m128i data_cmpresult[N_DATA];
__m128i data_rand[N_DATA];

void init_data()
{
    uint32_t rng = 42;
    volatile char dyn_zero = 0;  // Prevent constant-folding at -O3

    for (int i = 0; i < N_DATA; i++) {
        uint32_t r[4];
        for (int j = 0; j < 4; j++)
            r[j] = xorshift32(&rng);
        data_rand[i] = _mm_loadu_si128((const __m128i *) r);

        /* Derive other arrays using dyn_zero so they aren't compile-time
         * constants */
        data_zero[i] = _mm_set1_epi8((char) dyn_zero);
        data_all[i] = _mm_set1_epi8((char) (dyn_zero | 0xFF));

        char v80 = (char) (dyn_zero | 0x80);
        data_alt[i] = _mm_set_epi8((char) dyn_zero, v80, (char) dyn_zero, v80,
                                   (char) dyn_zero, v80, (char) dyn_zero, v80,
                                   (char) dyn_zero, v80, (char) dyn_zero, v80,
                                   (char) dyn_zero, v80, (char) dyn_zero, v80);

        char vFF = (char) (dyn_zero | 0xFF);
        data_cmpresult[i] = _mm_set_epi8(
            vFF, (char) dyn_zero, vFF, (char) dyn_zero, vFF, vFF,
            (char) dyn_zero, (char) dyn_zero, vFF, vFF, vFF, (char) dyn_zero,
            (char) dyn_zero, (char) dyn_zero, vFF, vFF);
    }
}

static void BM_Throughput_AllZero(benchmark::State &state)
{
    unsigned int acc[4] = {0};
    int i = 0;
    for (auto _ : state) {
        acc[i & 3] +=
            (unsigned int) _mm_movemask_epi8(data_zero[i & (N_DATA - 1)]);
        i++;
    }
    benchmark::DoNotOptimize(acc[0] + acc[1] + acc[2] + acc[3]);
}
BENCHMARK(BM_Throughput_AllZero);

static void BM_Throughput_AllOnes(benchmark::State &state)
{
    unsigned int acc[4] = {0};
    int i = 0;
    for (auto _ : state) {
        acc[i & 3] +=
            (unsigned int) _mm_movemask_epi8(data_all[i & (N_DATA - 1)]);
        i++;
    }
    benchmark::DoNotOptimize(acc[0] + acc[1] + acc[2] + acc[3]);
}
BENCHMARK(BM_Throughput_AllOnes);

static void BM_Throughput_Alternating(benchmark::State &state)
{
    unsigned int acc[4] = {0};
    int i = 0;
    for (auto _ : state) {
        acc[i & 3] +=
            (unsigned int) _mm_movemask_epi8(data_alt[i & (N_DATA - 1)]);
        i++;
    }
    benchmark::DoNotOptimize(acc[0] + acc[1] + acc[2] + acc[3]);
}
BENCHMARK(BM_Throughput_Alternating);

static void BM_Throughput_CmpResult(benchmark::State &state)
{
    unsigned int acc[4] = {0};
    int i = 0;
    for (auto _ : state) {
        acc[i & 3] +=
            (unsigned int) _mm_movemask_epi8(data_cmpresult[i & (N_DATA - 1)]);
        i++;
    }
    benchmark::DoNotOptimize(acc[0] + acc[1] + acc[2] + acc[3]);
}
BENCHMARK(BM_Throughput_CmpResult);

static void BM_Throughput_Random(benchmark::State &state)
{
    unsigned int acc[4] = {0};
    int i = 0;
    for (auto _ : state) {
        acc[i & 3] +=
            (unsigned int) _mm_movemask_epi8(data_rand[i & (N_DATA - 1)]);
        i++;
    }
    benchmark::DoNotOptimize(acc[0] + acc[1] + acc[2] + acc[3]);
}
BENCHMARK(BM_Throughput_Random);

static void BM_Latency(benchmark::State &state)
{
    __m128i vec = _mm_set1_epi8((char) 0xA5);
    for (auto _ : state) {
        int mask = _mm_movemask_epi8(vec);
        vec = _mm_set1_epi8((char) (mask & 0xFF));
    }
    benchmark::DoNotOptimize(vec);
}
BENCHMARK(BM_Latency);

uint8_t haystack[4096];

static void BM_Memchr_FoundAt2048(benchmark::State &state)
{
    memset(haystack, 0x42, sizeof(haystack));
    haystack[2048] = 0xAA;
    __m128i target = _mm_set1_epi8((char) 0xAA);
    int len = sizeof(haystack);
    for (auto _ : state) {
        int found_count = 0;
        for (int i = 0; i <= len - 16; i += 16) {
            __m128i chunk = _mm_loadu_si128((const __m128i *) (haystack + i));
            __m128i cmp = _mm_cmpeq_epi8(chunk, target);
            int mask = _mm_movemask_epi8(cmp);
            if (mask) {
                found_count++;
                break;
            }
        }
        benchmark::DoNotOptimize(found_count);
    }
}
BENCHMARK(BM_Memchr_FoundAt2048);

static void BM_Memchr_NotFound(benchmark::State &state)
{
    memset(haystack, 0x42, sizeof(haystack));
    __m128i target = _mm_set1_epi8((char) 0xAA);
    int len = sizeof(haystack);
    for (auto _ : state) {
        int found_count = 0;
        for (int i = 0; i <= len - 16; i += 16) {
            __m128i chunk = _mm_loadu_si128((const __m128i *) (haystack + i));
            __m128i cmp = _mm_cmpeq_epi8(chunk, target);
            int mask = _mm_movemask_epi8(cmp);
            if (mask) {
                found_count++;
                break;
            }
        }
        benchmark::DoNotOptimize(found_count);
    }
}
BENCHMARK(BM_Memchr_NotFound);

static void BM_Memchr_FoundAt0(benchmark::State &state)
{
    memset(haystack, 0x42, sizeof(haystack));
    haystack[0] = 0xAA;
    __m128i target = _mm_set1_epi8((char) 0xAA);
    int len = sizeof(haystack);
    for (auto _ : state) {
        int found_count = 0;
        for (int i = 0; i <= len - 16; i += 16) {
            __m128i chunk = _mm_loadu_si128((const __m128i *) (haystack + i));
            __m128i cmp = _mm_cmpeq_epi8(chunk, target);
            int mask = _mm_movemask_epi8(cmp);
            if (mask) {
                found_count++;
                break;
            }
        }
        benchmark::DoNotOptimize(found_count);
    }
}
BENCHMARK(BM_Memchr_FoundAt0);

int main(int argc, char **argv)
{
    init_data();
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}
