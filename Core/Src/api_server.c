//
// Created by meyti on 11/20/26.
//
// Raw lwIP TCP transport for the versioned JSON API.
//
// This file is transport-only: it owns the TCP lifecycle, HTTP framing, and
// response transmission. It knows nothing about GPIO, HAL, or application
// routes; those live in api_router.c, which it calls only once a *complete*
// request has been received. JSON serialization is in api_json.c.
//
// Request framing:
//   * A request is complete when its header block (terminated by CRLFCRLF) and
//     exactly Content-Length body bytes have arrived.
//   * Content-Length is validated (decimal, bounded, not duplicated). A value
//     larger than API_REQ_BODY_MAX is rejected with 413 before any body byte is
//     buffered.
//   * Transfer-Encoding is rejected: unsupported framing is never guessed, so
//     we can never desync from the client's idea of message boundaries.
//   * The body is stored in a fixed API_REQ_BODY_MAX buffer; nothing is
//     dispatched until the whole request is present, so a route handler never
//     observes a partial request.
//   * HEAD is resolved like GET (so headers and Content-Length are correct)
//     but the response body is suppressed.
//
// Execution context: lwIP callbacks run from the single NO_SYS=1 superloop
// (tud_task -> netif input -> lwIP). Nothing here may block or allocate.
//

#include <stdio.h>
#include <string.h>

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"

#include "api_benchmark.h"
#include "api_router.h"
#include "api_server.h"
#include "modules/acquisition.h"

#define API_PORT                 8080
#define API_MAX_CONNS            2
#define API_REQ_BUF_SIZE         512   /* maximum request-header bytes */
#define API_REQ_BODY_MAX         512   /* maximum request-body bytes */
#define API_BODY_BUF_SIZE        2048  /* router JSON response output */
#define API_RSP_BUF_SIZE         (API_BODY_BUF_SIZE + 256)
#define API_POLL_INTERVAL        2     /* lwIP poll interval, in TCP coarse
                                        * timer units (TCP_SLOW_INTERVAL =
                                        * 500 ms), so this is 1 s per poll */
#define API_RECV_TIMEOUT_POLLS   10    /* ~10 s receive inactivity */
#define API_SEND_TIMEOUT_POLLS   10    /* ~10 s response inactivity */
#define API_TOTAL_LIFETIME_POLLS 30    /* ~30 s hard cap from slot allocation,
                                        * never reset while the slot is in use */

/* Length of a NUL-terminated static body, excluding the terminator. */
#define BODY_LEN(s) ((uint16_t)(sizeof(s) - 1))

typedef enum
{
    API_CONN_IDLE = 0,
    API_CONN_HEADERS,  /* accumulating the request header block */
    API_CONN_BODY,     /* accumulating Content-Length body bytes */
    API_CONN_STREAM,   /* streaming a generated benchmark body */
    API_CONN_SEND      /* buffered response queued or being transmitted */
} api_conn_state_t;

typedef struct
{
    struct tcp_pcb *pcb;
    api_conn_state_t state;

    /* Bounded request-header accumulator. The extra byte guarantees room for
     * the NUL terminator we add when parsing. */
    char     req[API_REQ_BUF_SIZE + 1];
    uint16_t req_len;

    /* Parsed request line. method/path point into req and stay valid until the
     * slot is reused. */
    char    *method;
    char    *path;

    uint8_t  head_only;          /* response must carry headers but no body */
    uint8_t  has_content_length; /* 1 if the client supplied Content-Length */
    const char *content_type;    /* Content-Type value (into req), or NULL */
    uint16_t content_length;     /* declared request-body length (0 = none) */
    uint16_t req_body_len;       /* body bytes received so far */
    char     req_body[API_REQ_BODY_MAX];

    /* Router response output and the composed HTTP message. Both are fixed-size
     * and owned by the slot; conn_respond() copies the body into rsp once. */
    char     body[API_BODY_BUF_SIZE];
    char     rsp[API_RSP_BUF_SIZE];
    uint16_t rsp_len;
    uint16_t rsp_written;   /* bytes already handed to tcp_write() */
    uint32_t rsp_acked;     /* bytes acknowledged by the peer (header + body) */
    uint16_t idle_polls;    /* progress watchdog, reset on any progress */
    uint16_t age_polls;     /* total lifetime, never reset until slot reuse */

    /* Benchmark streaming state. `body` above is reused as the staging buffer
     * for generated chunks, so streaming needs no extra buffer. */
    uint32_t stream_total;      /* requested body bytes */
    uint32_t stream_written;    /* bytes accepted by tcp_write() */
    uint32_t stream_generated;  /* bytes staged into body (may exceed written) */
    uint16_t stream_retries;    /* sends that found no room or returned ERR_MEM */
    uint32_t stream_start_ms;   /* sys_now() when the stream began */
    const uint8_t *stream_ptr;  /* buffer source, or NULL for the generated pattern */
    uint8_t  bench_owned;       /* 1 while this slot holds the session gate */
    uint8_t  acq_owned;         /* 1 while this slot holds an acquisition view */
} api_conn_t;

static api_conn_t api_conns[API_MAX_CONNS];

/* Transport-level error bodies (routing errors live in api_router.c). */
static const char BODY_BAD_REQUEST[] =
    "{\"success\":false,\"error\":{"
    "\"code\":\"INVALID_REQUEST\",\"message\":\"Malformed HTTP request.\"}}";

static const char BODY_BAD_CONTENT_LENGTH[] =
    "{\"success\":false,\"error\":{"
    "\"code\":\"INVALID_CONTENT_LENGTH\",\"message\":\"Invalid Content-Length header.\"}}";

static const char BODY_UNSUPPORTED_TRANSFER[] =
    "{\"success\":false,\"error\":{"
    "\"code\":\"UNSUPPORTED_TRANSFER_ENCODING\",\"message\":\"Transfer-Encoding is not supported.\"}}";

static const char BODY_UNSUPPORTED_EXPECTATION[] =
    "{\"success\":false,\"error\":{"
    "\"code\":\"UNSUPPORTED_EXPECTATION\",\"message\":\"Expect header is not supported.\"}}";

static const char BODY_TOO_LARGE[] =
    "{\"success\":false,\"error\":{"
    "\"code\":\"REQUEST_TOO_LARGE\",\"message\":\"Request exceeds the size limit.\"}}";

static const char BODY_BENCH_BUSY[] =
    "{\"success\":false,\"error\":{"
    "\"code\":\"BENCHMARK_BUSY\",\"message\":\"A benchmark session is already running.\"}}";

static const char BODY_STREAM_BUSY[] =
    "{\"success\":false,\"error\":{"
    "\"code\":\"STREAM_BUSY\",\"message\":\"Another bulk transfer is in progress.\"}}";

static const char BODY_INTERNAL[] =
    "{\"success\":false,\"error\":{"
    "\"code\":\"INTERNAL_ERROR\",\"message\":\"Response too large.\"}}";

/* Finalize a benchmark session, if this slot owns one. Called from conn_reset()
 * so that normal completion, client FIN, client RST (api_err), send timeout and
 * explicit abort all funnel through one place. That is what makes the session
 * gate release exactly once: after this, bench_owned is 0. */
static void bench_finalize(api_conn_t *c)
{
    uint32_t response_total;
    uint32_t body_acked;
    uint32_t elapsed_ms;
    uint8_t  completed;

    if (!c->bench_owned)
    {
        return;
    }

    response_total = (uint32_t)c->rsp_len + c->stream_total;
    completed = (c->state == API_CONN_STREAM &&
                 c->rsp_written == c->rsp_len &&
                 c->stream_written == c->stream_total &&
                 c->rsp_acked >= response_total) ? 1u : 0u;
    elapsed_ms = sys_now() - c->stream_start_ms;

    /* Statistics report body bytes only, so a completed session shows
     * acked_bytes == requested_bytes; the HTTP header is not part of the
     * payload measurement. */
    body_acked = (c->rsp_acked > c->rsp_len) ? (c->rsp_acked - c->rsp_len) : 0u;
    if (body_acked > c->stream_total)
    {
        body_acked = c->stream_total;
    }

    /* One line per session, never per byte. */
    printf("BENCH requested=%lu generated=%lu written=%lu acked=%lu "
           "elapsed_ms=%lu retries=%u completed=%u\n",
           (unsigned long)c->stream_total,
           (unsigned long)c->stream_generated,
           (unsigned long)c->stream_written,
           (unsigned long)body_acked,
           (unsigned long)elapsed_ms,
           (unsigned)c->stream_retries,
           (unsigned)completed);

    API_Bench_Record(c->stream_total, body_acked, elapsed_ms,
                     c->stream_retries, completed);
    API_Bench_Release();
    c->bench_owned = 0;
}

/* Release an acquisition view if this slot holds one. Like bench_finalize this
 * runs from conn_reset(), so normal completion, client FIN/RST, send timeout
 * and explicit abort all hand the buffer back exactly once. */
static void acq_finalize(api_conn_t *c)
{
    if (!c->acq_owned)
    {
        return;
    }

    ACQ_EndRead();
    API_Bench_Release();   /* the shared bulk-stream gate */
    c->acq_owned = 0;
}

static void conn_reset(api_conn_t *c)
{
    bench_finalize(c);
    acq_finalize(c);

    c->pcb = NULL;
    c->state = API_CONN_IDLE;
    c->req_len = 0;
    c->method = NULL;
    c->path = NULL;
    c->head_only = 0;
    c->has_content_length = 0;
    c->content_type = NULL;
    c->content_length = 0;
    c->req_body_len = 0;
    c->rsp_len = 0;
    c->rsp_written = 0;
    c->rsp_acked = 0;
    c->idle_polls = 0;
    c->age_polls = 0;
    c->stream_total = 0;
    c->stream_written = 0;
    c->stream_generated = 0;
    c->stream_retries = 0;
    c->stream_start_ms = 0;
    c->stream_ptr = NULL;
}

/*
 * Graceful close: only used once all response bytes have been acknowledged,
 * or before any response was queued.
 *
 * Returns ERR_ABRT if tcp_close() failed and the PCB had to be aborted, so the
 * caller can honour the lwIP contract that a callback which aborts its PCB
 * must return ERR_ABRT. Returns ERR_OK otherwise.
 */
static err_t conn_close(api_conn_t *c)
{
    struct tcp_pcb *pcb = c->pcb;

    if (pcb != NULL)
    {
        /* Detach callbacks first so a late event cannot touch a freed slot. */
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);

        if (tcp_close(pcb) != ERR_OK)
        {
            tcp_abort(pcb);
            conn_reset(c);
            return ERR_ABRT;
        }
    }

    conn_reset(c);
    return ERR_OK;
}

/*
 * Hard close for error and timeout paths. Returns ERR_ABRT when a PCB was
 * aborted (so callbacks can propagate it to lwIP) and ERR_OK otherwise.
 */
static err_t conn_abort(api_conn_t *c)
{
    struct tcp_pcb *pcb = c->pcb;

    if (pcb != NULL)
    {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        tcp_abort(pcb);
        conn_reset(c);
        return ERR_ABRT;
    }

    conn_reset(c);
    return ERR_OK;
}

static api_conn_t *conn_alloc(void)
{
    for (uint8_t i = 0; i < API_MAX_CONNS; i++)
    {
        if (api_conns[i].state == API_CONN_IDLE)
        {
            conn_reset(&api_conns[i]);
            api_conns[i].state = API_CONN_HEADERS;
            return &api_conns[i];
        }
    }

    return NULL;
}

/* Hand as much of the response to lwIP as the send buffer allows.
 * ERR_MEM means "try again from tcp_sent()/tcp_poll()", not a failure. */
static err_t conn_try_send(api_conn_t *c)
{
    while (c->rsp_written < c->rsp_len)
    {
        uint16_t remaining = (uint16_t)(c->rsp_len - c->rsp_written);
        uint16_t room = tcp_sndbuf(c->pcb);
        uint16_t chunk;
        err_t err;

        if (room == 0)
        {
            return ERR_MEM;
        }

        chunk = (remaining < room) ? remaining : room;

        err = tcp_write(c->pcb, c->rsp + c->rsp_written, chunk,
                        TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM)
        {
            return ERR_MEM;
        }
        if (err != ERR_OK)
        {
            return err;
        }

        c->rsp_written = (uint16_t)(c->rsp_written + chunk);
    }

    /* tcp_write() and tcp_output() report separately. ERR_MEM from
     * tcp_output() means the segments are queued but could not go out yet
     * (for example the peer window is closed); retry from tcp_sent() /
     * tcp_poll(). Any other error is a real connection failure. */
    {
        err_t out = tcp_output(c->pcb);

        if (out == ERR_MEM)
        {
            return ERR_MEM;
        }
        if (out != ERR_OK)
        {
            return out;
        }
    }

    return ERR_OK;
}

/*
 * Streaming pump: first the fixed response header, then generated body bytes,
 * as much as the send buffer accepts. It never waits for acknowledgements;
 * ERR_MEM means "retry from tcp_sent()/tcp_poll()".
 *
 * sendbuf room does not guarantee tcp_write() success: lwIP can also return
 * ERR_MEM when segment or memory pools are exhausted. Both are treated as
 * retries. stream_written advances only after tcp_write() succeeds, and the
 * pattern is always regenerated from stream_written, so a chunk that failed to
 * queue is never counted twice nor written with the wrong offset.
 */
static err_t conn_try_stream(api_conn_t *c)
{
    for (;;)
    {
        uint16_t room = tcp_sndbuf(c->pcb);

        /* No room is ordinary backpressure, not a retry: tcp_sent()/tcp_poll()
         * call the pump again as the peer's window opens. Only a real ERR_MEM
         * from a write attempt is counted as a retry. */
        if (room == 0)
        {
            return ERR_MEM;
        }

        if (c->rsp_written < c->rsp_len)
        {
            uint16_t remaining = (uint16_t)(c->rsp_len - c->rsp_written);
            uint16_t chunk = (remaining < room) ? remaining : room;
            err_t err = tcp_write(c->pcb, c->rsp + c->rsp_written, chunk,
                                  TCP_WRITE_FLAG_COPY);

            if (err == ERR_MEM)
            {
                c->stream_retries++;
                return ERR_MEM;
            }
            if (err != ERR_OK)
            {
                return err;
            }

            c->rsp_written = (uint16_t)(c->rsp_written + chunk);
            continue;
        }

        if (c->stream_written < c->stream_total)
        {
            uint32_t remaining = c->stream_total - c->stream_written;
            uint16_t chunk = (remaining < room) ? (uint16_t)remaining : room;

            /* Direct buffer source (acquisition): stream straight from the
             * owning module's RAM. No staging copy and no pattern alignment. */
            if (c->stream_ptr != NULL)
            {
                err_t err = tcp_write(c->pcb,
                                      c->stream_ptr + c->stream_written, chunk,
                                      TCP_WRITE_FLAG_COPY);

                if (err == ERR_MEM)
                {
                    c->stream_retries++;
                    return ERR_MEM;
                }
                if (err != ERR_OK)
                {
                    return err;
                }

                c->stream_written += chunk;
                continue;
            }

            if (chunk > (uint16_t)sizeof c->body)
            {
                chunk = (uint16_t)sizeof c->body;
            }

            /* Keep the 4-byte counter words aligned across calls. Every allowed
             * size is a multiple of four, so no tail is ever stranded. */
            chunk = (uint16_t)(chunk & (uint16_t)~3u);
            if (chunk == 0)
            {
                return ERR_MEM;
            }

            API_Bench_Fill((uint8_t *)c->body, c->stream_written, chunk);
            c->stream_generated += chunk;

            {
                err_t err = tcp_write(c->pcb, c->body, chunk,
                                      TCP_WRITE_FLAG_COPY);

                if (err == ERR_MEM)
                {
                    c->stream_retries++;
                    return ERR_MEM;
                }
                if (err != ERR_OK)
                {
                    return err;
                }
            }

            c->stream_written += chunk;
            continue;
        }

        break; /* everything is queued */
    }

    {
        err_t out = tcp_output(c->pcb);

        if (out == ERR_MEM)
        {
            return ERR_MEM;
        }
        if (out != ERR_OK)
        {
            return out;
        }
    }

    return ERR_OK;
}

/* Compose and queue one HTTP response from a status and a body.
 *
 * Content-Length always reports the entity length (body_len). For a HEAD
 * request the body bytes are omitted but the header stays, matching what the
 * equivalent GET would have returned.
 *
 * Returns ERR_ABRT only if the connection was aborted, in which case the
 * caller must propagate ERR_ABRT to lwIP. */
static err_t conn_respond(api_conn_t *c, int status, const char *reason,
                          const char *extra_headers,
                          const char *body, uint16_t body_len)
{
    int n = snprintf(c->rsp, sizeof c->rsp,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     status, reason, (unsigned)body_len,
                     extra_headers != NULL ? extra_headers : "");

    if (n < 0 || (size_t)n >= sizeof c->rsp ||
        (size_t)n + body_len > sizeof c->rsp)
    {
        /* Defensive fallback: emit a known-small 500. */
        body = BODY_INTERNAL;
        body_len = BODY_LEN(BODY_INTERNAL);
        status = 500;
        reason = "Internal Server Error";
        extra_headers = NULL;

        n = snprintf(c->rsp, sizeof c->rsp,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason, (unsigned)body_len);

        if (n < 0 || (size_t)n >= sizeof c->rsp ||
            (size_t)n + body_len > sizeof c->rsp)
        {
            return conn_abort(c);
        }
    }

    {
        uint16_t hdr_len = (uint16_t)n;

        if (body_len > 0 && !c->head_only)
        {
            memcpy(c->rsp + hdr_len, body, body_len);
            c->rsp_len = (uint16_t)(hdr_len + body_len);
        }
        else
        {
            c->rsp_len = hdr_len;
        }
    }

    c->rsp_written = 0;
    c->rsp_acked = 0;
    c->idle_polls = 0;
    c->state = API_CONN_SEND;

    switch (conn_try_send(c))
    {
        case ERR_OK:
        case ERR_MEM:
            return ERR_OK;

        default:
            return conn_abort(c);
    }
}

static err_t respond_bad_request(api_conn_t *c)
{
    return conn_respond(c, 400, "Bad Request", NULL,
                        BODY_BAD_REQUEST, BODY_LEN(BODY_BAD_REQUEST));
}

static err_t respond_bad_content_length(api_conn_t *c)
{
    return conn_respond(c, 400, "Bad Request", NULL,
                        BODY_BAD_CONTENT_LENGTH,
                        BODY_LEN(BODY_BAD_CONTENT_LENGTH));
}

static err_t respond_unsupported_transfer(api_conn_t *c)
{
    return conn_respond(c, 400, "Bad Request", NULL,
                        BODY_UNSUPPORTED_TRANSFER,
                        BODY_LEN(BODY_UNSUPPORTED_TRANSFER));
}

static err_t respond_unsupported_expect(api_conn_t *c)
{
    return conn_respond(c, 400, "Bad Request", NULL,
                        BODY_UNSUPPORTED_EXPECTATION,
                        BODY_LEN(BODY_UNSUPPORTED_EXPECTATION));
}

static err_t respond_too_large(api_conn_t *c)
{
    return conn_respond(c, 413, "Payload Too Large", NULL,
                        BODY_TOO_LARGE, BODY_LEN(BODY_TOO_LARGE));
}

/* Case-insensitive equality for ASCII header names. */
static uint8_t ci_streq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a;
        char cb = *b;

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
        {
            return 0;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

typedef enum
{
    CL_OK = 0,
    CL_INVALID,
    CL_TOO_LARGE
} cl_result_t;

/* Parse a Content-Length value: decimal only, optional surrounding spaces,
 * nothing else. Values above API_REQ_BODY_MAX are CL_TOO_LARGE. */
static cl_result_t parse_content_length(const char *value, uint16_t *out)
{
    const char *s = value;
    uint32_t v = 0;
    uint8_t digits = 0;

    while (*s == ' ' || *s == '\t')
    {
        s++;
    }

    for (; *s >= '0' && *s <= '9'; s++)
    {
        digits = 1;
        /* Accumulate only while within range so the value can never overflow;
         * once it is over the limit the remaining digits are still validated. */
        if (v <= API_REQ_BODY_MAX)
        {
            v = v * 10u + (uint32_t)(*s - '0');
        }
    }

    if (!digits)
    {
        return CL_INVALID;
    }

    while (*s == ' ' || *s == '\t')
    {
        s++;
    }

    if (*s != '\0')
    {
        return CL_INVALID;
    }

    if (v > API_REQ_BODY_MAX)
    {
        return CL_TOO_LARGE;
    }

    *out = (uint16_t)v;
    return CL_OK;
}

/*
 * Compose the fixed response header for a streamed body and hand control to the
 * streaming pump. Content-Length is always the full body length. A HEAD request
 * sends only this header (through the ordinary buffered send path) and never
 * enters the streaming state, so it neither acquires the session gate nor
 * generates a body.
 */
static err_t conn_begin_stream(api_conn_t *c, uint32_t total,
                               const char *content_type)
{
    int n = snprintf(c->rsp, sizeof c->rsp,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     content_type != NULL ? content_type
                                          : "application/octet-stream",
                     (unsigned long)total);

    if (n < 0 || (size_t)n >= sizeof c->rsp)
    {
        return conn_abort(c);
    }

    c->rsp_len = (uint16_t)n;
    c->rsp_written = 0;
    c->rsp_acked = 0;
    c->idle_polls = 0;

    if (c->head_only)
    {
        c->state = API_CONN_SEND;

        switch (conn_try_send(c))
        {
            case ERR_OK:
            case ERR_MEM:
                return ERR_OK;

            default:
                return conn_abort(c);
        }
    }

    c->state = API_CONN_STREAM;

    switch (conn_try_stream(c))
    {
        case ERR_OK:
        case ERR_MEM:
            return ERR_OK;

        default:
            return conn_abort(c);
    }
}

/*
 * Buffer-backed stream (acquisition data). The router has already verified a
 * completed capture exists and set the Content-Length; here the transport takes
 * the shared bulk gate, pins the buffer (COMPLETE->STREAMING) and streams it
 * directly. HEAD sends only the header and touches neither the gate nor the
 * buffer.
 */
static err_t api_start_acq_stream(api_conn_t *c, const API_Route_t *meta)
{
    const uint16_t *samples = NULL;
    uint32_t count = 0;

    if (c->head_only)
    {
        return conn_begin_stream(c, meta->stream_bytes, meta->content_type);
    }

    if (!API_Bench_Acquire())
    {
        return conn_respond(c, 409, "Conflict", NULL,
                            BODY_STREAM_BUSY, BODY_LEN(BODY_STREAM_BUSY));
    }

    if (!ACQ_BeginRead(&samples, &count) || count == 0)
    {
        API_Bench_Release();
        return conn_respond(c, 409, "Conflict", NULL,
                            BODY_STREAM_BUSY, BODY_LEN(BODY_STREAM_BUSY));
    }

    c->acq_owned = 1;
    c->stream_ptr = (const uint8_t *)samples;
    c->stream_total = count * ACQ_SAMPLE_BYTES;
    c->stream_written = 0;
    c->stream_generated = 0;
    c->stream_retries = 0;
    c->stream_start_ms = sys_now();

    return conn_begin_stream(c, c->stream_total, meta->content_type);
}

static err_t api_start_stream(api_conn_t *c, const API_Route_t *meta)
{
    if (meta->stream_src == API_STREAM_BUFFER)
    {
        return api_start_acq_stream(c, meta);
    }

    if (c->head_only)
    {
        return conn_begin_stream(c, meta->stream_bytes, meta->content_type);
    }

    /* One session at a time. The router validated the size; the gate is
     * acquired here so the busy response is produced in exactly one place. */
    if (!API_Bench_Acquire())
    {
        return conn_respond(c, 409, "Conflict", NULL,
                            BODY_BENCH_BUSY, BODY_LEN(BODY_BENCH_BUSY));
    }

    c->bench_owned = 1;
    c->stream_total = meta->stream_bytes;
    c->stream_written = 0;
    c->stream_generated = 0;
    c->stream_retries = 0;
    c->stream_start_ms = sys_now();

    return conn_begin_stream(c, meta->stream_bytes, meta->content_type);
}

/* Resolve the (already complete) request through the router and queue the
 * response. Only valid once method/path have been parsed. */
static err_t api_dispatch(api_conn_t *c)
{
    API_Request_t req;
    API_Route_t meta;
    uint16_t body_len;

    req.body = c->req_body;
    req.body_len = c->req_body_len;
    req.body_present = c->has_content_length;
    req.content_type = c->content_type;

    body_len = API_Router_Handle(c->method, c->path, &req, &meta,
                                 c->body, (uint16_t)sizeof c->body);

    if (meta.stream_bytes > 0)
    {
        return api_start_stream(c, &meta);
    }

    return conn_respond(c, meta.status, meta.reason, meta.extra_headers,
                        c->body, body_len);
}

/*
 * Parse the request line and headers out of c->req (length c->req_len, already
 * known to end with CRLFCRLF) and decide what to do next:
 *   * on a framing error, a response is queued and the state becomes SEND;
 *   * if there is no body, the request is dispatched immediately;
 *   * otherwise the state becomes BODY and the caller feeds the remaining
 *     bytes of the current segment into the body.
 * Returns ERR_ABRT if the connection was aborted, ERR_OK otherwise.
 */
static err_t api_parse_and_start(api_conn_t *c)
{
    char *p = c->req;
    char *line_end;
    char *sp1;
    char *sp2;
    const char *version;
    uint8_t seen_content_length = 0;
    uint8_t seen_content_type = 0;

    c->req[c->req_len] = '\0';

    /* Request line: METHOD SP PATH SP VERSION */
    line_end = strstr(p, "\r\n");
    if (line_end == NULL)
    {
        return respond_bad_request(c);
    }
    *line_end = '\0';

    sp1 = strchr(p, ' ');
    if (sp1 == NULL)
    {
        return respond_bad_request(c);
    }
    *sp1 = '\0';
    c->method = p;
    c->path = sp1 + 1;

    sp2 = strchr(c->path, ' ');
    if (sp2 == NULL)
    {
        return respond_bad_request(c);
    }
    *sp2 = '\0';
    version = sp2 + 1;

    /* Only HTTP/1.0 and HTTP/1.1 are supported. Compare exactly so that
     * strings such as "HTTP/1.9" or "HTTP/1.1garbage" are rejected. */
    if (strcmp(version, "HTTP/1.0") != 0 &&
        strcmp(version, "HTTP/1.1") != 0)
    {
        return respond_bad_request(c);
    }

    c->head_only = (strcmp(c->method, "HEAD") == 0) ? 1 : 0;

    /* Header lines. */
    for (p = line_end + 2; *p != '\0'; p = line_end + 2)
    {
        char *colon;
        char *name;
        const char *value;

        line_end = strstr(p, "\r\n");
        if (line_end == NULL)
        {
            return respond_bad_request(c);
        }
        *line_end = '\0';

        if (*p == '\0')
        {
            break; /* blank line: end of headers */
        }

        colon = strchr(p, ':');
        if (colon == NULL)
        {
            return respond_bad_request(c);
        }
        *colon = '\0';

        name = p;
        value = colon + 1;

        /* A field name must be non-empty and contain no whitespace. */
        if (name[0] == '\0' || strchr(name, ' ') != NULL ||
            strchr(name, '\t') != NULL)
        {
            return respond_bad_request(c);
        }

        while (*value == ' ' || *value == '\t')
        {
            value++;
        }

        if (ci_streq(name, "Expect"))
        {
            /* 100-continue is not implemented. Refusing explicitly is better
             * than waiting for a body the client holds back until it sees a
             * "100 Continue" that never comes. */
            return respond_unsupported_expect(c);
        }

        if (ci_streq(name, "Transfer-Encoding"))
        {
            /* We only accept identity framing via Content-Length. Chunked (and
             * anything else) is refused rather than guessed, so message
             * boundaries can never be misinterpreted. */
            return respond_unsupported_transfer(c);
        }

        if (ci_streq(name, "Content-Type"))
        {
            if (seen_content_type)
            {
                return respond_bad_request(c);
            }
            /* Points into req, which stays valid until dispatch. The router
             * decides whether the media type is acceptable for the route. */
            c->content_type = value;
            seen_content_type = 1;
        }

        if (ci_streq(name, "Content-Length"))
        {
            cl_result_t cl;

            if (seen_content_length)
            {
                return respond_bad_content_length(c);
            }

            cl = parse_content_length(value, &c->content_length);
            if (cl == CL_INVALID)
            {
                return respond_bad_content_length(c);
            }
            if (cl == CL_TOO_LARGE)
            {
                return respond_too_large(c);
            }

            seen_content_length = 1;
            c->has_content_length = 1;
        }
    }

    if (c->content_length == 0)
    {
        return api_dispatch(c);
    }

    c->state = API_CONN_BODY;
    return ERR_OK;
}

/*
 * Returns 1 if `buf` (length `len`) ends with the CRLFCRLF header terminator.
 * Called after appending a single byte, so it only inspects the last four.
 */
static uint8_t header_complete(const char *buf, uint16_t len)
{
    return len >= 4 &&
           buf[len - 4] == '\r' && buf[len - 3] == '\n' &&
           buf[len - 2] == '\r' && buf[len - 1] == '\n';
}

static err_t api_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    api_conn_t *c = (api_conn_t *)arg;
    uint16_t total;
    uint16_t offset = 0;
    err_t result = ERR_OK;

    if (c == NULL)
    {
        if (p != NULL)
        {
            pbuf_free(p);
        }
        return ERR_OK;
    }

    /* Verified against lwIP 2.1.2 in this tree: TCP_EVENT_RECV is only ever
     * invoked with ERR_OK (tcp_in.c tcp_input, tcp.c tcp_process_refused_data),
     * and a remote FIN arrives as p == NULL with ERR_OK via TCP_EVENT_CLOSED.
     * Resets are delivered through tcp_err instead. This branch is therefore
     * defensive: it keeps the transport correct if a future lwIP version starts
     * reporting receive errors here. */
    if (err != ERR_OK)
    {
        if (p != NULL)
        {
            pbuf_free(p);
        }
        return conn_abort(c);
    }

    if (p == NULL)
    {
        /* Remote FIN. If a response is still going out, let it finish;
         * otherwise there is nothing to serve. A half-closed client that keeps
         * reading must still receive the whole stream. */
        if (c->state != API_CONN_SEND && c->state != API_CONN_STREAM)
        {
            return conn_close(c);
        }
        return ERR_OK;
    }

    total = p->tot_len;
    tcp_recved(pcb, total);

    /* Walk the segment as a byte stream. Headers are accumulated one byte at a
     * time so we never copy body bytes into the header buffer; once the header
     * block ends, the loop switches to body accumulation and consumes the rest
     * of the same segment. */
    while (offset < total)
    {
        if (c->state == API_CONN_HEADERS)
        {
            if (c->req_len >= API_REQ_BUF_SIZE)
            {
                result = respond_too_large(c);
                break;
            }

            if (pbuf_copy_partial(p, &c->req[c->req_len], 1, offset) != 1)
            {
                result = conn_abort(c);
                break;
            }

            c->req_len++;
            offset++;
            c->idle_polls = 0;

            if (header_complete(c->req, c->req_len))
            {
                result = api_parse_and_start(c);
                if (result == ERR_ABRT || c->state == API_CONN_SEND)
                {
                    break; /* response queued, or aborting */
                }
                /* state is now BODY; keep consuming this segment */
            }
        }
        else if (c->state == API_CONN_BODY)
        {
            uint16_t remaining = (uint16_t)(c->content_length - c->req_body_len);
            uint16_t avail = (uint16_t)(total - offset);
            uint16_t take = (avail < remaining) ? avail : remaining;

            if (take == 0)
            {
                break;
            }

            if (pbuf_copy_partial(p, c->req_body + c->req_body_len, take,
                                  offset) != take)
            {
                result = conn_abort(c);
                break;
            }

            c->req_body_len = (uint16_t)(c->req_body_len + take);
            offset = (uint16_t)(offset + take);
            c->idle_polls = 0;

            if (c->req_body_len == c->content_length)
            {
                result = api_dispatch(c);
                break;
            }

            break; /* need the rest of the body in a later segment */
        }
        else
        {
            /* Response in flight: ignore/discard any further request bytes. */
            break;
        }
    }

    pbuf_free(p);

    return (result == ERR_ABRT) ? ERR_ABRT : ERR_OK;
}

static err_t api_sent(void *arg, struct tcp_pcb *pcb, uint16_t len)
{
    api_conn_t *c = (api_conn_t *)arg;

    (void)pcb;

    if (c == NULL ||
        (c->state != API_CONN_SEND && c->state != API_CONN_STREAM))
    {
        return ERR_OK;
    }

    c->rsp_acked += len;
    c->idle_polls = 0;

    if (c->state == API_CONN_STREAM)
    {
        uint32_t total = (uint32_t)c->rsp_len + c->stream_total;

        if (c->rsp_written < c->rsp_len ||
            c->stream_written < c->stream_total)
        {
            err_t err = conn_try_stream(c);

            if (err != ERR_OK && err != ERR_MEM)
            {
                return conn_abort(c);
            }
        }

        /* Close only after the header and every body byte have been queued and
         * acknowledged, so no unsent data is discarded. */
        if (c->rsp_written == c->rsp_len &&
            c->stream_written == c->stream_total &&
            c->rsp_acked >= total)
        {
            return conn_close(c);
        }

        return ERR_OK;
    }

    if (c->rsp_written < c->rsp_len)
    {
        err_t err = conn_try_send(c);

        if (err != ERR_OK && err != ERR_MEM)
        {
            return conn_abort(c);
        }
    }

    /* Close only after every response byte has been acknowledged, so no
     * unsent data is discarded. */
    if (c->rsp_written == c->rsp_len && c->rsp_acked >= c->rsp_len)
    {
        return conn_close(c);
    }

    return ERR_OK;
}

static err_t api_poll(void *arg, struct tcp_pcb *pcb)
{
    api_conn_t *c = (api_conn_t *)arg;

    (void)pcb;

    if (c == NULL)
    {
        return ERR_OK;
    }

    c->idle_polls++;
    c->age_polls++;

    /* Hard total-lifetime backstop. Unlike idle_polls, age_polls is never
     * reset, so a client that keeps making just-in-time progress still has a
     * bounded lifetime. Abort a response that is still in flight so queued
     * bytes are not silently discarded; otherwise close gracefully. */
    if (c->age_polls >= API_TOTAL_LIFETIME_POLLS)
    {
        uint8_t incomplete = 0;

        if (c->state == API_CONN_SEND)
        {
            incomplete = (c->rsp_acked < c->rsp_len);
        }
        else if (c->state == API_CONN_STREAM)
        {
            incomplete = (c->stream_written < c->stream_total ||
                          c->rsp_acked < (uint32_t)c->rsp_len + c->stream_total);
        }

        if (incomplete)
        {
            return conn_abort(c);
        }
        return conn_close(c);
    }

    if (c->state == API_CONN_HEADERS || c->state == API_CONN_BODY)
    {
        if (c->idle_polls >= API_RECV_TIMEOUT_POLLS)
        {
            return conn_close(c);
        }
        return ERR_OK;
    }

    if (c->state == API_CONN_STREAM)
    {
        uint32_t total = (uint32_t)c->rsp_len + c->stream_total;

        if (c->rsp_written < c->rsp_len ||
            c->stream_written < c->stream_total)
        {
            err_t err = conn_try_stream(c);

            if (err != ERR_OK && err != ERR_MEM)
            {
                return conn_abort(c);
            }
        }
        else if (c->rsp_acked < total)
        {
            /* All bytes are queued; re-run tcp_output() in case an earlier
             * call returned ERR_MEM because the window was closed. */
            err_t out = tcp_output(c->pcb);

            if (out != ERR_OK && out != ERR_MEM)
            {
                return conn_abort(c);
            }
        }

        if (c->rsp_written == c->rsp_len &&
            c->stream_written == c->stream_total &&
            c->rsp_acked >= total)
        {
            return conn_close(c);
        }

        if (c->idle_polls >= API_SEND_TIMEOUT_POLLS)
        {
            return conn_abort(c);
        }

        return ERR_OK;
    }

    if (c->state == API_CONN_SEND)
    {
        if (c->rsp_written < c->rsp_len)
        {
            err_t err = conn_try_send(c);

            if (err != ERR_OK && err != ERR_MEM)
            {
                return conn_abort(c);
            }
        }
        else if (c->rsp_acked < c->rsp_len)
        {
            /* All bytes are queued; re-run tcp_output() in case an earlier
             * call returned ERR_MEM because the window was closed. */
            err_t out = tcp_output(c->pcb);

            if (out != ERR_OK && out != ERR_MEM)
            {
                return conn_abort(c);
            }
        }

        if (c->rsp_written == c->rsp_len && c->rsp_acked >= c->rsp_len)
        {
            return conn_close(c);
        }

        if (c->idle_polls >= API_SEND_TIMEOUT_POLLS)
        {
            return conn_abort(c);
        }
    }

    return ERR_OK;
}

static void api_err(void *arg, err_t err)
{
    api_conn_t *c = (api_conn_t *)arg;

    (void)err;

    if (c != NULL)
    {
        /* lwIP has already freed the pcb; only release our slot. */
        conn_reset(c);
    }
}

static err_t api_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    api_conn_t *c;

    (void)arg;

    if (err != ERR_OK || newpcb == NULL)
    {
        return ERR_VAL;
    }

    c = conn_alloc();
    if (c == NULL)
    {
        /* Pool exhausted: returning non-ERR_OK makes lwIP abort this PCB. */
        return ERR_MEM;
    }

    /* Bind the slot to the accepted PCB. Without this, all later send/close
     * paths operate on a NULL pcb and the connection can never be answered
     * or released. */
    c->pcb = newpcb;

    tcp_arg(newpcb, c);
    tcp_recv(newpcb, api_recv);
    tcp_sent(newpcb, api_sent);
    tcp_err(newpcb, api_err);
    tcp_poll(newpcb, api_poll, API_POLL_INTERVAL);
    tcp_nagle_disable(newpcb);

    return ERR_OK;
}

void API_Server_Init(void)
{
    struct tcp_pcb *pcb;
    struct tcp_pcb *lpcb;

    memset(api_conns, 0, sizeof api_conns);

    pcb = tcp_new();
    if (pcb == NULL)
    {
        printf("ERROR: API server: tcp_new failed\n");
        return;
    }

    if (tcp_bind(pcb, IP_ADDR_ANY, API_PORT) != ERR_OK)
    {
        printf("ERROR: API server: tcp_bind(%u) failed\n", API_PORT);
        tcp_close(pcb);
        return;
    }

    lpcb = tcp_listen(pcb);
    if (lpcb == NULL)
    {
        printf("ERROR: API server: tcp_listen failed\n");
        /* tcp_listen() frees the original pcb only on success. */
        tcp_close(pcb);
        return;
    }

    tcp_arg(lpcb, NULL);
    tcp_accept(lpcb, api_accept);

    printf("API server listening on port %u\n", API_PORT);
}
