//
// Created by meyti on 11/20/26.
//
// USB NCM binary throughput benchmark: deterministic payload generation and
// session accounting. This module holds no transport state and performs no I/O;
// the transport (api_server.c) pumps bytes and calls in here for the pattern
// and for session bookkeeping.
//

#ifndef USB_WEBSERVER_API_BENCHMARK_H
#define USB_WEBSERVER_API_BENCHMARK_H

#include <stdint.h>

/* Fixed, allowed transfer sizes. Anything else is rejected by the router. */
uint8_t API_Bench_IsAllowedSize(uint32_t bytes);

/*
 * Session gate. At most one benchmark stream runs at a time so a transfer
 * cannot occupy both connection slots and starve normal API traffic.
 * Acquire() returns 1 on success, 0 when a session is already active.
 * Release() must be called exactly once per successful Acquire().
 */
uint8_t API_Bench_Acquire(void);
void    API_Bench_Release(void);

/*
 * Fill dst[0..len) with the deterministic payload at absolute body offset
 * `offset`. The pattern is a 32-bit little-endian counter: the word at body
 * offset 4*w is w, low byte first. `offset` is always a multiple of 4 and
 * `len` is always a multiple of 4, so a counter word never straddles a call.
 */
void API_Bench_Fill(uint8_t *dst, uint32_t offset, uint16_t len);

/* Last session statistics. `valid` is 0 until the first session is recorded. */
typedef struct
{
    uint32_t requested_bytes;
    uint32_t acked_bytes;
    uint32_t elapsed_ms;
    uint16_t retries;
    uint8_t  completed;  /* 1 only if every requested byte was acknowledged */
    uint8_t  valid;
} API_BenchStats_t;

/*
 * Record the outcome of a session. Called once per session from the transport
 * teardown path; the values persist after the connection closes.
 */
void API_Bench_Record(uint32_t requested_bytes, uint32_t acked_bytes,
                      uint32_t elapsed_ms, uint16_t retries, uint8_t completed);

const API_BenchStats_t *API_Bench_GetStats(void);

#endif // USB_WEBSERVER_API_BENCHMARK_H
