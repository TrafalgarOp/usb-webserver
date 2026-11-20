//
// Created by meyti on 11/20/26.
//

#include "api_benchmark.h"

/* The single-session gate and the last recorded statistics live here so the
 * transport only has to ask; nothing in this file touches lwIP. */
static uint8_t          bench_busy = 0;
static API_BenchStats_t bench_last;

uint8_t API_Bench_IsAllowedSize(uint32_t bytes)
{
    switch (bytes)
    {
        case 4096:
        case 16384:
        case 32768:
        case 65536:
        case 131072:
            return 1;

        default:
            return 0;
    }
}

uint8_t API_Bench_Acquire(void)
{
    if (bench_busy)
    {
        return 0;
    }

    bench_busy = 1;
    return 1;
}

void API_Bench_Release(void)
{
    bench_busy = 0;
}

void API_Bench_Fill(uint8_t *dst, uint32_t offset, uint16_t len)
{
    uint32_t word = offset >> 2;
    uint16_t i = 0;

    while (i < len)
    {
        dst[i + 0] = (uint8_t)(word & 0xFFu);
        dst[i + 1] = (uint8_t)((word >> 8) & 0xFFu);
        dst[i + 2] = (uint8_t)((word >> 16) & 0xFFu);
        dst[i + 3] = (uint8_t)((word >> 24) & 0xFFu);
        word++;
        i = (uint16_t)(i + 4u);
    }
}

void API_Bench_Record(uint32_t requested_bytes, uint32_t acked_bytes,
                      uint32_t elapsed_ms, uint16_t retries, uint8_t completed)
{
    bench_last.requested_bytes = requested_bytes;
    bench_last.acked_bytes = acked_bytes;
    bench_last.elapsed_ms = elapsed_ms;
    bench_last.retries = retries;
    bench_last.completed = completed ? 1u : 0u;
    bench_last.valid = 1;
}

const API_BenchStats_t *API_Bench_GetStats(void)
{
    return &bench_last;
}
