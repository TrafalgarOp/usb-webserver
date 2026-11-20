//
// Created by meyti on 11/20/26.
//
// Application router for the versioned JSON API.
//
// Routing, validation, and hardware application. The two mutation handlers
// validate the method, route, media type, JSON body, pin, and pin state before
// calling the GPIO module through the apply_*() functions; a module failure is
// never reported as success.
//

#include <stdio.h>
#include <string.h>

#include "api.h"
#include "api_benchmark.h"
#include "api_json.h"
#include "api_router.h"
#include "modules/gpio.h"

/* ------------------------------------------------------------------------- */
/* Value names                                                                */
/* ------------------------------------------------------------------------- */

static const char *mode_name(GPIO_Mode_t mode)
{
    return mode == GPIO_CONFIG_OUTPUT ? "output" : "input";
}

static const char *pull_name(GPIO_Pull_t pull)
{
    switch (pull)
    {
        case GPIO_CONFIG_PULL_UP:
            return "up";

        case GPIO_CONFIG_PULL_DOWN:
            return "down";

        default:
            return "none";
    }
}

/* ------------------------------------------------------------------------- */
/* Error responses                                                            */
/* ------------------------------------------------------------------------- */

static uint16_t respond_internal(API_Route_t *meta, char *body, uint16_t cap)
{
    JSON_Writer_t w;

    JSON_Init(&w, body, cap);
    JSON_Raw(&w, "{\"success\":false,\"error\":{\"code\":\"INTERNAL_ERROR\","
                 "\"message\":\"Response buffer overflow.\"}}");

    meta->status = 500;
    meta->reason = "Internal Server Error";
    meta->extra_headers = NULL;

    return JSON_Len(&w);
}

static uint16_t respond_error(API_Route_t *meta, char *body, uint16_t cap,
                              int status, const char *reason,
                              const char *extra_headers,
                              const char *code, const char *message)
{
    JSON_Writer_t w;

    JSON_Init(&w, body, cap);
    JSON_Raw(&w, "{\"success\":false,\"error\":{\"code\":");
    JSON_String(&w, code);
    JSON_Raw(&w, ",\"message\":");
    JSON_String(&w, message);
    JSON_Raw(&w, "}}");

    if (JSON_Overflowed(&w))
    {
        return respond_internal(meta, body, cap);
    }

    meta->status = status;
    meta->reason = reason;
    meta->extra_headers = extra_headers;

    return JSON_Len(&w);
}

static uint16_t err_invalid_json(API_Route_t *meta, char *body, uint16_t cap,
                                 const char *message)
{
    return respond_error(meta, body, cap, 400, "Bad Request", NULL,
                         "INVALID_JSON", message);
}

static uint16_t err_invalid_value(API_Route_t *meta, char *body, uint16_t cap,
                                  const char *message)
{
    return respond_error(meta, body, cap, 400, "Bad Request", NULL,
                         "INVALID_VALUE", message);
}

static uint16_t err_invalid_pin(API_Route_t *meta, char *body, uint16_t cap)
{
    return respond_error(meta, body, cap, 400, "Bad Request", NULL,
                         "INVALID_PIN", "Malformed pin name.");
}

static uint16_t err_invalid_query(API_Route_t *meta, char *body, uint16_t cap,
                                  const char *message)
{
    return respond_error(meta, body, cap, 400, "Bad Request", NULL,
                         "INVALID_QUERY", message);
}

static uint16_t err_pin_not_found(API_Route_t *meta, char *body, uint16_t cap,
                                  const char *name)
{
    char message[48];

    snprintf(message, sizeof message, "Pin %s is not addressable.", name);
    return respond_error(meta, body, cap, 404, "Not Found", NULL,
                         "PIN_NOT_FOUND", message);
}

static uint16_t err_media_type(API_Route_t *meta, char *body, uint16_t cap)
{
    return respond_error(meta, body, cap, 415, "Unsupported Media Type", NULL,
                         "UNSUPPORTED_MEDIA_TYPE",
                         "Content-Type must be application/json.");
}

static uint16_t err_state_conflict(API_Route_t *meta, char *body, uint16_t cap,
                                   const char *message)
{
    return respond_error(meta, body, cap, 409, "Conflict", NULL,
                         "HARDWARE_STATE_CONFLICT", message);
}

static uint16_t err_gpio_failed(API_Route_t *meta, char *body, uint16_t cap)
{
    return respond_error(meta, body, cap, 500, "Internal Server Error", NULL,
                         "GPIO_OPERATION_FAILED", "Hardware operation failed.");
}

static uint16_t err_method(API_Route_t *meta, char *body, uint16_t cap,
                           const char *allow)
{
    return respond_error(meta, body, cap, 405, "Method Not Allowed", allow,
                         "METHOD_NOT_ALLOWED", "Method not allowed for this route.");
}

/* ------------------------------------------------------------------------- */
/* Pin serialization                                                          */
/* ------------------------------------------------------------------------- */

/* Write "P<port><pin>" (for example "PA5" or "PB12"). buf must hold 5 bytes. */
static void pin_name(char *buf, uint8_t port, uint8_t pin)
{
    buf[0] = 'P';
    buf[1] = (char)('A' + port);

    if (pin >= 10)
    {
        buf[2] = (char)('0' + (pin / 10));
        buf[3] = (char)('0' + (pin % 10));
        buf[4] = '\0';
    }
    else
    {
        buf[2] = (char)('0' + pin);
        buf[3] = '\0';
    }
}

/* mode, pull, and value are emitted only for configured pins: an unconfigured
 * pin must not look like an input with pull-none and value 0. */
static void write_pin_object(JSON_Writer_t *w, uint8_t port, uint8_t pin,
                             const GPIO_PinInfo_t *info)
{
    char name[5];

    pin_name(name, port, pin);

    JSON_Raw(w, "{\"pin\":");
    JSON_String(w, name);
    JSON_Raw(w, ",\"configured\":");
    JSON_Bool(w, info->configured);

    if (info->configured)
    {
        JSON_Raw(w, ",\"mode\":");
        JSON_String(w, mode_name(info->mode));
        JSON_Raw(w, ",\"pull\":");
        JSON_String(w, pull_name(info->pull));
        JSON_Raw(w, ",\"value\":");
        JSON_UInt(w, info->value);
    }

    JSON_Raw(w, "}");
}

static uint16_t respond_pin_ok(API_Route_t *meta, char *body, uint16_t cap,
                               uint8_t port, uint8_t pin,
                               const GPIO_PinInfo_t *info)
{
    JSON_Writer_t w;

    JSON_Init(&w, body, cap);
    JSON_Raw(&w, "{\"success\":true,\"data\":");
    write_pin_object(&w, port, pin, info);
    JSON_Raw(&w, "}");

    if (JSON_Overflowed(&w))
    {
        return respond_internal(meta, body, cap);
    }

    meta->status = 200;
    meta->reason = "OK";
    meta->extra_headers = NULL;

    return JSON_Len(&w);
}

/* ------------------------------------------------------------------------- */
/* Pin-name parsing                                                           */
/* ------------------------------------------------------------------------- */

typedef enum
{
    PIN_PARSE_OK = 0,
    PIN_PARSE_MALFORMED
} pin_parse_t;

/*
 * Accept exactly "P[A|B]<0-15>":
 *   PA5, PB12   -> OK
 *   PA16, PC5, PA, pa5, PA5x -> malformed
 * A syntactically valid pin that the whitelist rejects (PA0, PA9, ...) is not
 * malformed; the caller turns that into 404.
 */
static pin_parse_t parse_pin(const char *s, uint8_t *port, uint8_t *pin)
{
    size_t len = strlen(s);
    uint8_t value;

    if (len < 3 || len > 4)
    {
        return PIN_PARSE_MALFORMED;
    }

    if (s[0] != 'P')
    {
        return PIN_PARSE_MALFORMED;
    }

    if (s[1] == 'A')
    {
        *port = GPIO_PORT_A;
    }
    else if (s[1] == 'B')
    {
        *port = GPIO_PORT_B;
    }
    else
    {
        return PIN_PARSE_MALFORMED;
    }

    if (len == 3)
    {
        if (s[2] < '0' || s[2] > '9')
        {
            return PIN_PARSE_MALFORMED;
        }

        value = (uint8_t)(s[2] - '0');
    }
    else
    {
        if (s[2] < '0' || s[2] > '9' || s[3] < '0' || s[3] > '9')
        {
            return PIN_PARSE_MALFORMED;
        }

        value = (uint8_t)((s[2] - '0') * 10 + (s[3] - '0'));
    }

    if (value > 15)
    {
        return PIN_PARSE_MALFORMED;
    }

    *pin = value;
    return PIN_PARSE_OK;
}

/* ------------------------------------------------------------------------- */
/* Read routes                                                                */
/* ------------------------------------------------------------------------- */

static uint16_t route_system(API_Route_t *meta, char *body, uint16_t cap)
{
    JSON_Writer_t w;

    JSON_Init(&w, body, cap);
    JSON_Raw(&w, "{\"success\":true,\"data\":{\"name\":\"usb-webserver\","
                 "\"api_version\":\"1\",\"firmware\":\"");
    JSON_Raw(&w, API_FIRMWARE_VERSION);
    JSON_Raw(&w, "\"}}");

    if (JSON_Overflowed(&w))
    {
        return respond_internal(meta, body, cap);
    }

    meta->status = 200;
    meta->reason = "OK";
    meta->extra_headers = NULL;

    return JSON_Len(&w);
}

static uint16_t route_gpio_list(API_Route_t *meta, char *body, uint16_t cap)
{
    JSON_Writer_t w;
    uint8_t first = 1;

    JSON_Init(&w, body, cap);
    JSON_Raw(&w, "{\"success\":true,\"data\":{\"pins\":[");

    for (uint8_t port = 0; port < GPIO_PORT_COUNT; port++)
    {
        for (uint8_t pin = 0; pin < 16; pin++)
        {
            GPIO_PinInfo_t info;

            if (!GPIO_IsPinAllowed(port, pin))
            {
                continue;
            }

            if (GPIO_GetPinInfo(port, pin, &info) != GPIO_STATUS_OK)
            {
                continue;
            }

            if (!first)
            {
                JSON_Raw(&w, ",");
            }
            first = 0;

            write_pin_object(&w, port, pin, &info);
        }
    }

    JSON_Raw(&w, "]}}");

    if (JSON_Overflowed(&w))
    {
        return respond_internal(meta, body, cap);
    }

    meta->status = 200;
    meta->reason = "OK";
    meta->extra_headers = NULL;

    return JSON_Len(&w);
}

static uint16_t route_gpio_item_read(const char *name, API_Route_t *meta,
                                     char *body, uint16_t cap)
{
    uint8_t port = 0;
    uint8_t pin = 0;
    GPIO_PinInfo_t info;

    if (parse_pin(name, &port, &pin) != PIN_PARSE_OK)
    {
        return err_invalid_pin(meta, body, cap);
    }

    if (!GPIO_IsPinAllowed(port, pin))
    {
        return err_pin_not_found(meta, body, cap, name);
    }

    if (GPIO_GetPinInfo(port, pin, &info) != GPIO_STATUS_OK)
    {
        return respond_internal(meta, body, cap);
    }

    return respond_pin_ok(meta, body, cap, port, pin, &info);
}

/* ------------------------------------------------------------------------- */
/* Media type                                                                 */
/* ------------------------------------------------------------------------- */

/* Accept "application/json", case-insensitively, with optional parameters
 * ("; charset=utf-8") and surrounding whitespace. */
static uint8_t is_json_content_type(const char *ct)
{
    static const char json[] = "application/json";

    if (ct == NULL)
    {
        return 0;
    }

    while (*ct == ' ' || *ct == '\t')
    {
        ct++;
    }

    for (uint8_t i = 0; json[i] != '\0'; i++)
    {
        char c = ct[i];

        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }

        if (c != json[i])
        {
            return 0;
        }
    }

    /* The char after the media type must end the value or start a parameter. */
    {
        char c = ct[sizeof(json) - 1];
        return c == '\0' || c == ';' || c == ' ' || c == '\t';
    }
}

/* ------------------------------------------------------------------------- */
/* Hardware application                                                       */
/* ------------------------------------------------------------------------- */

/*
 * These two functions are the only places the router touches the GPIO module.
 * Both run only after the full validation chain (method, route, media type,
 * JSON structure, field values, allowlist, and for writes the output-mode
 * precondition). A non-OK result is mapped to 500 GPIO_OPERATION_FAILED and can
 * never be reported as success.
 *
 * The GPIO module updates its tracking arrays only after the HAL call, so a
 * failed operation cannot leave partially updated software metadata.
 */
static GPIO_Status_t apply_configuration(uint8_t port, uint8_t pin,
                                         GPIO_Mode_t mode, GPIO_Pull_t pull)
{
    return GPIO_ConfigurePin(port, pin, mode, pull);
}

static GPIO_Status_t apply_value(uint8_t port, uint8_t pin, uint8_t value)
{
    return GPIO_WritePin(port, pin, value);
}

/* ------------------------------------------------------------------------- */
/* Mutation routes                                                            */
/* ------------------------------------------------------------------------- */

/*
 * PUT /api/v1/gpio/{pin}  body {"mode":"input|output"[,"pull":"none|up|down"]}
 *
 * Ordering: pin syntax -> media type -> JSON structure -> field values -> pin
 * allowlist -> hardware. Nothing before the seam touches the hardware.
 */
static uint16_t route_gpio_configure(const char *name, const API_Request_t *req,
                                     API_Route_t *meta, char *body, uint16_t cap)
{
    uint8_t port = 0;
    uint8_t pin = 0;
    GPIO_Mode_t mode = GPIO_CONFIG_INPUT;
    GPIO_Pull_t pull = GPIO_CONFIG_PULL_NONE;
    char mode_str[8] = {0};
    char pull_str[8] = {0};
    uint8_t seen_mode = 0;
    uint8_t seen_pull = 0;
    JSON_Reader_t r;
    JSON_Member_t m;
    GPIO_PinInfo_t info;

    if (parse_pin(name, &port, &pin) != PIN_PARSE_OK)
    {
        return err_invalid_pin(meta, body, cap);
    }

    if (!is_json_content_type(req->content_type))
    {
        return err_media_type(meta, body, cap);
    }

    if (!req->body_present)
    {
        return err_invalid_json(meta, body, cap, "Request body is required.");
    }
    if (req->body_len == 0)
    {
        return err_invalid_json(meta, body, cap, "Request body is empty.");
    }

    JSON_ReaderInit(&r, req->body, req->body_len);

    if (!JSON_ReadObjectStart(&r))
    {
        return err_invalid_json(meta, body, cap, "Malformed JSON.");
    }

    for (;;)
    {
        char key[16];

        m = JSON_ReadKey(&r, key, sizeof key);
        if (m == JSON_MEMBER_ERROR)
        {
            return err_invalid_json(meta, body, cap, "Malformed JSON.");
        }
        if (m == JSON_MEMBER_END)
        {
            break;
        }

        if (strcmp(key, "mode") == 0)
        {
            if (seen_mode)
            {
                return err_invalid_json(meta, body, cap, "Duplicate field: mode.");
            }
            if (!JSON_ReadStringValue(&r, mode_str, sizeof mode_str))
            {
                return err_invalid_json(meta, body, cap, "Field mode must be a string.");
            }
            if (JSON_ReaderTruncated(&r))
            {
                return err_invalid_value(meta, body, cap, "Unsupported mode.");
            }
            seen_mode = 1;
        }
        else if (strcmp(key, "pull") == 0)
        {
            if (seen_pull)
            {
                return err_invalid_json(meta, body, cap, "Duplicate field: pull.");
            }
            if (!JSON_ReadStringValue(&r, pull_str, sizeof pull_str))
            {
                return err_invalid_json(meta, body, cap, "Field pull must be a string.");
            }
            if (JSON_ReaderTruncated(&r))
            {
                return err_invalid_value(meta, body, cap, "Unsupported pull.");
            }
            seen_pull = 1;
        }
        else
        {
            char msg[40];

            snprintf(msg, sizeof msg, "Unknown field: %s.", key);
            return err_invalid_json(meta, body, cap, msg);
        }
    }

    if (!JSON_ReadDone(&r))
    {
        return err_invalid_json(meta, body, cap, "Trailing data after JSON object.");
    }

    if (!seen_mode)
    {
        return err_invalid_json(meta, body, cap, "Missing required field: mode.");
    }

    if (strcmp(mode_str, "input") == 0)
    {
        mode = GPIO_CONFIG_INPUT;
    }
    else if (strcmp(mode_str, "output") == 0)
    {
        mode = GPIO_CONFIG_OUTPUT;
    }
    else
    {
        return err_invalid_value(meta, body, cap, "Unsupported mode.");
    }

    if (seen_pull)
    {
        if (strcmp(pull_str, "none") == 0)
        {
            pull = GPIO_CONFIG_PULL_NONE;
        }
        else if (strcmp(pull_str, "up") == 0)
        {
            pull = GPIO_CONFIG_PULL_UP;
        }
        else if (strcmp(pull_str, "down") == 0)
        {
            pull = GPIO_CONFIG_PULL_DOWN;
        }
        else
        {
            return err_invalid_value(meta, body, cap, "Unsupported pull.");
        }
    }

    /* Capability gate: only the GPIO module's whitelist decides what is
     * addressable. Every allowed pin supports both modes and all pulls. */
    if (!GPIO_IsPinAllowed(port, pin))
    {
        return err_pin_not_found(meta, body, cap, name);
    }

    if (apply_configuration(port, pin, mode, pull) != GPIO_STATUS_OK)
    {
        return err_gpio_failed(meta, body, cap);
    }

    /* Success: report the authoritative module state, never a guess. */
    if (GPIO_GetPinInfo(port, pin, &info) != GPIO_STATUS_OK)
    {
        return err_gpio_failed(meta, body, cap);
    }

    return respond_pin_ok(meta, body, cap, port, pin, &info);
}

/*
 * PUT /api/v1/gpio/{pin}/value  body {"value":0|1}
 *
 * Same ordering as above, plus a state precondition: the pin must already be a
 * configured output, else 409.
 */
static uint16_t route_gpio_value(const char *name, const API_Request_t *req,
                                 API_Route_t *meta, char *body, uint16_t cap)
{
    uint8_t port = 0;
    uint8_t pin = 0;
    int32_t value = 0;
    uint8_t seen_value = 0;
    JSON_Reader_t r;
    JSON_Member_t m;
    GPIO_PinInfo_t info;

    if (parse_pin(name, &port, &pin) != PIN_PARSE_OK)
    {
        return err_invalid_pin(meta, body, cap);
    }

    if (!is_json_content_type(req->content_type))
    {
        return err_media_type(meta, body, cap);
    }

    if (!req->body_present)
    {
        return err_invalid_json(meta, body, cap, "Request body is required.");
    }
    if (req->body_len == 0)
    {
        return err_invalid_json(meta, body, cap, "Request body is empty.");
    }

    JSON_ReaderInit(&r, req->body, req->body_len);

    if (!JSON_ReadObjectStart(&r))
    {
        return err_invalid_json(meta, body, cap, "Malformed JSON.");
    }

    for (;;)
    {
        char key[16];

        m = JSON_ReadKey(&r, key, sizeof key);
        if (m == JSON_MEMBER_ERROR)
        {
            return err_invalid_json(meta, body, cap, "Malformed JSON.");
        }
        if (m == JSON_MEMBER_END)
        {
            break;
        }

        if (strcmp(key, "value") == 0)
        {
            if (seen_value)
            {
                return err_invalid_json(meta, body, cap, "Duplicate field: value.");
            }
            if (!JSON_ReadIntValue(&r, &value))
            {
                return err_invalid_json(meta, body, cap, "Field value must be an integer.");
            }
            seen_value = 1;
        }
        else
        {
            char msg[40];

            snprintf(msg, sizeof msg, "Unknown field: %s.", key);
            return err_invalid_json(meta, body, cap, msg);
        }
    }

    if (!JSON_ReadDone(&r))
    {
        return err_invalid_json(meta, body, cap, "Trailing data after JSON object.");
    }

    if (!seen_value)
    {
        return err_invalid_json(meta, body, cap, "Missing required field: value.");
    }

    /* Only the exact integers 0 and 1 are accepted: not 2, not -1, not "1",
     * not true. */
    if (value != 0 && value != 1)
    {
        return err_invalid_value(meta, body, cap, "Field value must be 0 or 1.");
    }

    if (!GPIO_IsPinAllowed(port, pin))
    {
        return err_pin_not_found(meta, body, cap, name);
    }

    /* State gate: writing requires an already-configured output. */
    if (GPIO_GetPinInfo(port, pin, &info) != GPIO_STATUS_OK)
    {
        return err_gpio_failed(meta, body, cap);
    }
    if (!info.configured || info.mode != GPIO_CONFIG_OUTPUT)
    {
        return err_state_conflict(meta, body, cap,
                                  "Pin is not a configured output.");
    }

    if (apply_value(port, pin, (uint8_t)value) != GPIO_STATUS_OK)
    {
        return err_gpio_failed(meta, body, cap);
    }

    if (GPIO_GetPinInfo(port, pin, &info) != GPIO_STATUS_OK)
    {
        return err_gpio_failed(meta, body, cap);
    }

    return respond_pin_ok(meta, body, cap, port, pin, &info);
}

/* ------------------------------------------------------------------------- */
/* Benchmark routes                                                           */
/* ------------------------------------------------------------------------- */

/* Parse "bytes=<decimal>" with no other parameters. Bounded so the accumulator
 * can never overflow; an oversized value simply fails the allowlist check. */
static uint8_t parse_query_bytes(const char *query, uint32_t *out)
{
    const char *s = query;
    uint32_t v = 0;
    uint8_t digits = 0;

    if (strncmp(s, "bytes=", 6) != 0)
    {
        return 0;
    }
    s += 6;

    for (; *s >= '0' && *s <= '9'; s++)
    {
        digits = 1;
        if (v <= 1000000u)
        {
            v = v * 10u + (uint32_t)(*s - '0');
        }
    }

    if (!digits || *s != '\0')
    {
        return 0;
    }

    *out = v;
    return 1;
}

/*
 * GET/HEAD /api/v1/benchmark/stream?bytes=N
 *
 * The router only validates and describes the transfer: it sets stream_bytes
 * and leaves the body empty. The transport composes the header and streams the
 * payload. HEAD is handled by the transport exactly like a streamed GET header
 * but without acquiring the session gate.
 */
static uint16_t route_bench_stream(const char *method, const char *query,
                                   API_Route_t *meta, char *body, uint16_t cap)
{
    uint32_t bytes = 0;

    (void)method;
    (void)body;
    (void)cap;

    if (query == NULL || query[0] == '\0')
    {
        return err_invalid_query(meta, body, cap, "Missing bytes parameter.");
    }

    if (!parse_query_bytes(query, &bytes))
    {
        return err_invalid_query(meta, body, cap, "Malformed bytes parameter.");
    }

    if (!API_Bench_IsAllowedSize(bytes))
    {
        return err_invalid_value(meta, body, cap, "Unsupported size.");
    }

    meta->status = 200;
    meta->reason = "OK";
    meta->extra_headers = NULL;
    meta->stream_bytes = bytes;
    meta->content_type = "application/octet-stream";
    return 0; /* no buffered body */
}

/* GET/HEAD /api/v1/benchmark/stats — last recorded session. */
static uint16_t route_bench_stats(API_Route_t *meta, char *body, uint16_t cap)
{
    const API_BenchStats_t *s = API_Bench_GetStats();
    JSON_Writer_t w;

    if (!s->valid)
    {
        return respond_error(meta, body, cap, 404, "Not Found", NULL,
                             "NO_SESSION", "No benchmark session has run.");
    }

    JSON_Init(&w, body, cap);
    JSON_Raw(&w, "{\"success\":true,\"data\":{\"requested_bytes\":");
    JSON_UInt(&w, s->requested_bytes);
    JSON_Raw(&w, ",\"acked_bytes\":");
    JSON_UInt(&w, s->acked_bytes);
    JSON_Raw(&w, ",\"elapsed_ms\":");
    JSON_UInt(&w, s->elapsed_ms);
    JSON_Raw(&w, ",\"retries\":");
    JSON_UInt(&w, s->retries);
    JSON_Raw(&w, ",\"completed\":");
    JSON_Bool(&w, s->completed);
    JSON_Raw(&w, "}}");

    if (JSON_Overflowed(&w))
    {
        return respond_internal(meta, body, cap);
    }

    meta->status = 200;
    meta->reason = "OK";
    meta->extra_headers = NULL;
    return JSON_Len(&w);
}

/* ------------------------------------------------------------------------- */
/* Route resolution and dispatch                                              */
/* ------------------------------------------------------------------------- */

typedef enum
{
    ROUTE_UNKNOWN = 0,
    ROUTE_SYSTEM,
    ROUTE_GPIO_LIST,
    ROUTE_GPIO_ITEM,    /* /api/v1/gpio/{pin} */
    ROUTE_GPIO_VALUE,   /* /api/v1/gpio/{pin}/value */
    ROUTE_BENCH_STREAM, /* /api/v1/benchmark/stream[?bytes=N] */
    ROUTE_BENCH_STATS   /* /api/v1/benchmark/stats */
} route_shape_t;

/*
 * Identify the route shape from the path. The path is owned by the caller and
 * may be split in place when a "/value" suffix is present, or when the
 * benchmark stream route carries a query string. On return, *pin_name points
 * into path for the item/value shapes (else NULL) and *query points at the
 * bytes after '?' for the benchmark stream (else NULL).
 */
static route_shape_t resolve_shape(char *path, const char **pin_name,
                                   char **query)
{
    *pin_name = NULL;
    *query = NULL;

    if (strcmp(path, "/api/v1/system") == 0)
    {
        return ROUTE_SYSTEM;
    }

    if (strcmp(path, "/api/v1/gpio") == 0)
    {
        return ROUTE_GPIO_LIST;
    }

    if (strcmp(path, "/api/v1/benchmark/stats") == 0)
    {
        return ROUTE_BENCH_STATS;
    }

    /* The stream route is the only one that accepts a query string. A '?' on
     * any other route leaves the path unmatched, exactly as before. */
    if (strcmp(path, "/api/v1/benchmark/stream") == 0)
    {
        return ROUTE_BENCH_STREAM;
    }

    if (strncmp(path, "/api/v1/benchmark/stream?", 25) == 0)
    {
        path[24] = '\0'; /* split "stream" from the query */
        *query = path + 25;
        return ROUTE_BENCH_STREAM;
    }

    if (strncmp(path, "/api/v1/gpio/", 13) == 0)
    {
        char *rest = path + 13;
        char *slash;

        if (*rest == '\0')
        {
            return ROUTE_UNKNOWN;
        }

        slash = strchr(rest, '/');
        if (slash == NULL)
        {
            *pin_name = rest;
            return ROUTE_GPIO_ITEM;
        }

        if (slash != rest && strcmp(slash, "/value") == 0)
        {
            *slash = '\0'; /* split "<pin>" from "value" */
            *pin_name = rest;
            return ROUTE_GPIO_VALUE;
        }

        return ROUTE_UNKNOWN;
    }

    return ROUTE_UNKNOWN;
}

static uint8_t is_read_method(const char *method)
{
    return strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0;
}

uint16_t API_Router_Handle(const char *method, char *path,
                           const API_Request_t *req,
                           API_Route_t *meta, char *body, uint16_t cap)
{
    const char *pin_name = NULL;
    char *query = NULL;
    route_shape_t shape;

    /* Streaming is opt-in per route: default every response to buffered JSON so
     * the transport never reads an uninitialized descriptor. */
    meta->stream_bytes = 0;
    meta->content_type = NULL;

    shape = resolve_shape(path, &pin_name, &query);

    switch (shape)
    {
        case ROUTE_SYSTEM:
            if (!is_read_method(method))
            {
                return err_method(meta, body, cap, "Allow: GET, HEAD\r\n");
            }
            return route_system(meta, body, cap);

        case ROUTE_GPIO_LIST:
            if (!is_read_method(method))
            {
                return err_method(meta, body, cap, "Allow: GET, HEAD\r\n");
            }
            return route_gpio_list(meta, body, cap);

        case ROUTE_GPIO_ITEM:
            if (is_read_method(method))
            {
                return route_gpio_item_read(pin_name, meta, body, cap);
            }
            if (strcmp(method, "PUT") == 0)
            {
                return route_gpio_configure(pin_name, req, meta, body, cap);
            }
            return err_method(meta, body, cap, "Allow: GET, HEAD, PUT\r\n");

        case ROUTE_GPIO_VALUE:
            if (strcmp(method, "PUT") == 0)
            {
                return route_gpio_value(pin_name, req, meta, body, cap);
            }
            return err_method(meta, body, cap, "Allow: PUT\r\n");

        case ROUTE_BENCH_STREAM:
            if (!is_read_method(method))
            {
                return err_method(meta, body, cap, "Allow: GET, HEAD\r\n");
            }
            return route_bench_stream(method, query, meta, body, cap);

        case ROUTE_BENCH_STATS:
            if (!is_read_method(method))
            {
                return err_method(meta, body, cap, "Allow: GET, HEAD\r\n");
            }
            return route_bench_stats(meta, body, cap);

        case ROUTE_UNKNOWN:
        default:
            return respond_error(meta, body, cap, 404, "Not Found", NULL,
                                 "NOT_FOUND", "Unknown endpoint.");
    }
}
