# Evolving the usb-webserver API: a practical, incremental tutorial

This tutorial is written against the **actual code in this repository**, not against an
idealized design. It explains what exists today, why the current proof of concept cannot
grow into the desired REST API, and how to build the replacement one small, testable step
at a time.

The target reader knows basic C and embedded development (HAL, interrupts, a superloop)
but has not designed an HTTP API on a microcontroller before.

---

## How to read this document

Two labels are used throughout:

- **[EXISTING]** — a file, function, behaviour or limitation that is really in the repo
  today. You can verify these with the `file:line` references given in Chapter 0 and 1.
- **[PROPOSED]** — a suggested addition. **Every symbol marked PROPOSED is new work**;
  it does not exist in the repository yet. Do not assume a function is available just
  because it appears here.

Rules that are assumed throughout:

1. `Core/Src/gpio.c` + `Core/Inc/gpio.h` are CubeMX output — leave them alone.
2. `lib/tinyusb/**` and `lib/lwip/**` are git submodules — do not patch them.
3. Hardware lives in `Core/Src/modules/**`; transport lives in the API layer.
4. `NO_SYS=1`, one cooperative loop, no RTOS, no blocking waits, no `malloc`.

---

## Chapter 0 — Verified repository facts

Before designing anything, here is what the code actually does. (All line numbers refer
to the state of the repo when this tutorial was written.)

### 0.1 Hardware and clock

- MCU: **STM32F411CEU6**, UFQFPN48 (`usb-webserver.ioc:14-15`), HSE 25 MHz → 96 MHz
  (`Core/Src/main.c:202-241`), nominal **512 KB flash / 128 KB SRAM**.
- CubeMX-managed pins (`usb-webserver.ioc:16-19`): `PC13` and `PB0` as GPIO outputs,
  `PH0/PH1` for HSE. USB pins `PA9`, `PA10`, `PA11`, `PA12` are configured **by hand** in
  `MX_TinyUSB_Init()` (`Core/Src/main.c:60-106`), inside a `USER CODE` block.
- `PB0` is the "board LED" preserved by the GPIO module (`Core/Src/modules/gpio.c:64-70`);
  `PC13` is blinked by `LED_Status_Task()` (`Core/Src/main.c:108-135`):
  - 250 ms → USB not mounted
  - 1000 ms → mounted
  - 2500 ms → suspended

  Keep this in mind when debugging: a frozen LED pattern usually means the superloop is
  stuck.

### 0.2 Startup and execution model

`main()` (`Core/Src/main.c:149-196`):

```c
MX_GPIO_Init();          /* CubeMX */
MX_TinyUSB_Init();       /* user code: USB OTG FS + TinyUSB */
usb_network_init();      /* lwIP netif */
usb_network_services_init(); /* DHCP + API + HTTPD */
Hardware_Init();

while (1) {
    tud_task();
    sys_check_timeouts();
    LED_Status_Task();
    Hardware_Task();
}
```

There is **no RTOS**. `tud_task()` is where NCM frames arrive; `tud_network_recv_cb()`
(`Core/Src/usb_network.c:208-239`) hands them to `netif->input()`, which runs TCP/IP and
therefore all lwIP callbacks. `sys_check_timeouts()` drives lwIP timers. Consequences:

- Anything slow inside an lwIP callback stalls USB reception and the whole device.
- Never call `tud_task()` or `sys_check_timeouts()` from inside another callback
  (the existing `linkoutput_fn()` at `usb_network.c:76-89` calls `tud_task()` only when it
  must drain a transmit, which is the sanctioned pattern — do not add more nesting).
- All API work must be **state machines**, never blocking reads.

Note: `printf()` calls exist (`usb_network.c:217`, `:230`, `:253`) but `Core/Src/syscalls.c:80-90`
routes output to a weak `__io_putchar()` that the project does not implement strongly, so
today the messages effectively go nowhere. If you want logs, add a strong `__io_putchar()`
(e.g. ITM/SWO) in a **new** file rather than editing CubeMX output.

### 0.3 Network stack (already working)

- `Core/Src/usb_network.c`: TinyUSB **NCM** (`Core/Inc/tusb_config.h:159-160`), lwIP
  **2.1.2**, static `192.168.7.1/24`, gateway `0.0.0.0`, virtual MAC `02:02:84:6A:96:00`
  (netif uses `...01`).
- `Core/Inc/lwipopts.h`: `NO_SYS=1`, `LWIP_SOCKET=0`, `LWIP_NETCONN=0`, `LWIP_RAW=0`,
  `LWIP_DHCP=0`, `LWIP_HTTPD_CGI=1`, `LWIP_HTTPD_SSI=0`, `HTTPD_USE_CUSTOM_FSDATA=1`.
  `LWIP_HIGH_THROUGHPUT` is **1** for STM32F4 via `Core/Inc/tusb_config.h:107-119`, so
  `TCP_WND=8*TCP_MSS`, `PBUF_POOL_SIZE=8`, `MEM_SIZE≈8.7 KB` (`lwipopts.h:70-85`). Memory
  is available but not unlimited — measure before adding buffers.
- **HTTPD options that are not set anywhere** (so lwIP defaults apply): `LWIP_HTTPD_SUPPORT_POST=0`,
  `LWIP_HTTPD_DYNAMIC_HEADERS=0`, `LWIP_HTTPD_SUPPORT_EXTSTATUS=0`,
  `LWIP_HTTPD_SUPPORT_11_KEEPALIVE=0`, `LWIP_HTTPD_MAX_CGI_PARAMETERS=16`
  (`httpd_opts.h`). `DYNAMIC_HEADERS=0` is why the status line and `Content-Type` live
  *inside* the generated `fsdata_custom.c` for each file, and why there is no generatable
  404/501 page. See Appendix C for the runtime consequences.
- `Core/Src/dhserver.c`: small raw-UDP DHCP server (Sergey Fetisov style) with **three**
  leases `.2`, `.3`, `.4` configured in `usb_network.c:51-66`. DNS is deliberately empty:
  the host would otherwise rank this link first and stall lookups (`usb_network.c:61`).
  It is *not* a general DHCP range — only three hosts can get an address.

**Practical consequence for testing:** if `192.168.7.2` does not appear on the host,
renew the DHCP lease / recreate the interface. Do not debug the API until ping works.

### 0.4 Static web files

- `web/index.html`, `web/js/api.js`, `web/css/*.css` are compiled into `fsdata_custom.c`
  at build time by `makefsdata` (`CMakeLists.txt:107-168`). The generated file is a C
  array of bytes containing name + ready-made HTTP headers + file body (see
  `fsdata_custom.c:30-55`).
- Therefore **editing HTML requires a rebuild**; editing files under `lib/` does not.
- There is no build-time JSON/REST anything yet.

### 0.5 Hardware modules and the existing API

`Core/Inc/hardware_config.h`:

```c
#define ENABLE_GPIO  1
#define ENABLE_ADC   0
#define ENABLE_PWM   0
#define ENABLE_UART  0
```

`Core/Src/hardware.c` is a thin compile-time dispatcher: `Hardware_Init()` → `GPIO_Init()`,
`Hardware_Task()` → `GPIO_Task()`, `Hardware_GPIO_SetLED()` → `GPIO_SetLED()`.

`Core/Inc/modules/gpio.h` **[EXISTING]** exposes exactly:

```c
void      GPIO_Init(void);
void      GPIO_Task(void);
GPIO_Status_t GPIO_ConfigurePin(uint8_t port, uint8_t pin, GPIO_Mode_t mode, GPIO_Pull_t pull);
GPIO_Status_t GPIO_WritePin(uint8_t port, uint8_t pin, uint8_t state);
GPIO_Status_t GPIO_ReadPin(uint8_t port, uint8_t pin, uint8_t *state);
void      GPIO_SetLED(uint8_t state);
```

with `GPIO_Mode_t {INPUT, OUTPUT}`, `GPIO_Pull_t {NONE, UP, DOWN}` and
`GPIO_Status_t {OK, INVALID_PIN, INVALID_MODE, ERROR}`.

Internal state in `Core/Src/modules/gpio.c` **[EXISTING]**:

```c
static uint8_t     pin_configured[2][16];
static GPIO_Mode_t pin_mode[2][16];
static uint8_t     GPIO_IsPinAllowed(uint8_t port, uint8_t pin); /* private! */
```

Allowed pins (`gpio.c:34-47`): **PA1–PA8, PA15, PB0–PB10, PB12–PB15**. PA0, PA9–PA14 and
all port-C pins are *not* available through this module (PA9–PA12 are USB). Pull state is
**not stored** anywhere, and there is no "deconfigure" function. Those are the first real
gaps we will fill in Chapter 5.

### 0.6 Suggested new file layout (proposed)

```
Core/
├── Inc/
│   ├── api.h                  [EXISTING] keep as the public entry point
│   ├── api_server.h           [PROPOSED] raw TCP HTTP server + CORS
│   ├── api_router.h           [PROPOSED] method/path dispatch table
│   ├── api_json.h             [PROPOSED] bounded JSON reader/writer
│   ├── api_errors.h           [PROPOSED] error codes → HTTP status
│   ├── events.h               [PROPOSED] fixed-size domain event queue
│   ├── api_ws.h               [PROPOSED] WebSocket protocol (later)
│   └── modules/
│       ├── gpio.h             [EXISTING] extend, do not replace
│       ├── adc.h / pwm.h / uart.h / i2c.h / spi.h   [PROPOSED]
├── Src/
│   ├── api.c                  [EXISTING] becomes a thin Init façade
│   ├── api_server.c           [PROPOSED]
│   ├── api_router.c           [PROPOSED]
│   ├── api_json.c             [PROPOSED]
│   ├── api_errors.c           [PROPOSED]
│   ├── events.c               [PROPOSED]
│   ├── api_ws.c               [PROPOSED]
│   ├── modules/gpio.c         [EXISTING] extend
│   └── ...
web/js/api.js                  [EXISTING] rewrite to fetch() + WebSocket
CMakeLists.txt                 [EXISTING] add the new .c files to target_sources()
```

Add every new `Core/Src/*.c` to `CMakeLists.txt` (the `add_executable()` list at lines
35-42 or the `target_sources()` block at lines 53-57). A forgotten source file produces a
confusing "undefined reference" at link time, not at compile time.

---

## Chapter 1 — Understanding the current API

### 1.1 The path of a request today

```text
Host browser
   │  GET /api?device=gpio&port=1&pin=0&state=1
   ▼
USB NCM ──► tud_task() ──► tud_network_recv_cb() ──► netif->input()
                                                        │
                                                        ▼
                                              lwIP TCP → httpd.c
                                                        │
                                  http_parse_request(): only "GET " or "POST "
                                                        │
                                  http_find_file(): split URI at '?',
                                  strcmp() base URI with registered CGI names
                                                        │
                                  api_cgi_handler(...) in Core/Src/api.c
                                                        │
                                  GPIO_ConfigurePin() / GPIO_WritePin()
                                                        │
                                  return "/index.html"
                                                        ▼
                                  httpd: fs_open("/index.html") → 200 OK + HTML
```

Read the exact code: `Core/Src/api.c:15-115` is the handler, `:117-120` the CGI table,
`:124-132` the registration.

### 1.2 How registration works

`API_Init()` is called from `usb_network_services_init()`
(`Core/Src/usb_network.c:249-258`) **before** `httpd_init()`:

```c
/* Core/Src/api.c:124-132 [EXISTING] */
void API_Init(void)
{
#if LWIP_HTTPD_CGI
    http_set_cgi_handlers(cgi_handlers,
        sizeof(cgi_handlers) / sizeof(cgi_handlers[0]));
#endif
}
```

`http_set_cgi_handlers()` stores an array of `{ "/api", function }`. Later,
`http_find_file()` (`lib/lwip/src/apps/http/httpd.c:2246-2263`) does:

```c
params = (char *)strchr(uri, '?');   /* split query string off */
if (params != NULL) { *params = '\0'; params++; }
for (i = 0; i < httpd_num_cgis; i++) {
    if (strcmp(uri, httpd_cgis[i].pcCGIName) == 0) {
        http_cgi_paramcount = extract_uri_parameters(hs, params);
        uri = httpd_cgis[i].pfnCGIHandler(i, http_cgi_paramcount,
                                          hs->params, hs->param_vals);
        break;
    }
}
err = fs_open(&hs->file_handle, uri);
```

Two non-obvious facts:

1. **The return value is a filename, not an HTTP redirect.** Returning `"/index.html"`
   makes httpd open that file and serve it with `200 OK`. The browser address bar still
   shows `/api?...`. No `Location:` header is sent.
2. The match is an **exact `strcmp`** on the base URI. `/api` works; `/api/`, `/api/v1`,
   `/api?...` with a trailing slash in the registered name would not.

### 1.3 What the handler actually does

Walking `Core/Src/api.c:30-111`:

- Loops over `num_params`, copying pointers for `device`, `port`, `pin`, `state`, `mode`,
  `pull`.
- Requires `device == "gpio"`, `port`, `pin`.
- Uses `strtol()` with an end-pointer check and ranges `port ∈ [0,1]`, `pin ∈ [0,15]`.
  Note it validates the *numeric range*, **not** the module whitelist: `PA9` passes this
  check and then fails inside `GPIO_IsPinAllowed()` with `GPIO_STATUS_INVALID_PIN`, which
  the handler ignores.
- If `mode` is present, maps `"input"`/`"output"` and `up`/`down`/`none`, then calls
  `GPIO_ConfigurePin()`.
- If `state` is present, maps only exactly `"0"` or `"1"` and calls `GPIO_WritePin()`.
  Any other value is **silently ignored** — a bug.
- Reads back the pin twice (`api.c:99-108`) and discards the result; the return status of
  `GPIO_ReadPin()` is ignored and `status` is never updated from it.
- Always returns `"/index.html"`.

`api.c` includes `hardware.h` but never uses it — a leftover. `<stdlib.h>` is there for
`strtol()`.

### 1.4 Why this cannot become the REST API

The ceiling is in lwIP HTTPD itself, not in `api.c`:

| Requirement | HTTPD/CGI reality |
|---|---|
| `PUT`, `DELETE`, `PATCH` | `http_parse_request()` (`httpd.c:2020-2040`) recognizes only `GET` and (if `LWIP_HTTPD_SUPPORT_POST`, **default 0** in `httpd_opts.h:125-126`) `POST`. Everything else calls `http_find_error_file(hs, 501)`, which is a no-op macro returning `ERR_ARG` because `LWIP_HTTPD_SUPPORT_EXTSTATUS=0` (`httpd.c:1661-1663`); `http_recv()` then just closes the connection (`httpd.c:2584-2586`). **The client gets no HTTP response at all** — `curl` reports *Empty reply from server* (verified on the board, Appendix C), not a 501/405. |
| JSON body | POST support is off. Even if enabled, `httpd_post_receive_data()` streams body chunks to your callback; the CGI handler itself never sees a body. |
| Status codes (201/204/400/409/413/415/500) | CGI can only choose a file name; status is decided by `http_init_file()`/`fs_open()`. There is no API to set it. |
| `Content-Type: application/json` | Comes from the embedded HTTP header in `fsdata_custom.c`. You would need to add a fake `.json` file to the generated filesystem. |
| Structured error envelope | No hook to write a body from the handler. |
| Unknown path → error page | `LWIP_HTTPD_DYNAMIC_HEADERS=0` (default, `httpd_opts.h:170-171`) and no `/404.html` in the filesystem, so an unknown path also produces **no response** (verified, Appendix C). The `get_http_headers()` 404 branch (`httpd.c:857-872`) is not even compiled. |
| WebSocket `Upgrade` | No `Upgrade:` parsing, no callback to take ownership of the TCP connection. You would have to patch `lib/lwip/src/apps/http/httpd.c`. |
| URL-decoding of parameters | `extract_uri_parameters()` (`httpd.c:688-744`) only splits on `&` and `=`; no percent-decoding. |
| Parameter count | Bounded by `LWIP_HTTPD_MAX_CGI_PARAMETERS` (default **16**, `httpd_opts.h:130-131`); values are raw pointers into the request buffer. |

**Conclusion:** keep lwIP HTTPD for what it is good at — serving the generated static
files on port 80 — and move the API to its own transport. Expanding CGI would mean
patching a submodule and still fighting its "handler returns a filename" model.

### 1.5 Limitations to carry into the design

| Today | Target |
|---|---|
| Query parameters | JSON body, `Content-Type: application/json` |
| GET only | GET / PUT / DELETE (+ OPTIONS for CORS) |
| Always HTML body | JSON envelope with `success` / `data` / `error` |
| No status codes | 200/201/204/400/404/405/409/413/415/500 |
| No error format | `{ code, message }` |
| Pin as `port`+`pin` integers | Canonical names `PA5`, `PB0` |
| Errors silently ignored | Every hardware status mapped to an HTTP result |
| No notifications | WebSocket event distribution |

---

## Chapter 2 — Designing the API architecture

### 2.1 What "REST" means on this device

Keep it deliberately small:

- **Resources** are nouns in the path: `/system`, `/gpio`, `/gpio/PA5`.
- **Methods** are verbs: `GET` reads, `PUT` creates or replaces configuration, `DELETE`
  removes configuration.
- **Version** is in the path: `/api/v1`. Changing the contract later becomes `/api/v2`
  without breaking old clients.
- **Stateless**: each request carries everything needed; the server does not remember
  "what the browser was doing".
- **Representation** is JSON with one consistent envelope.

This is not a database. `PUT /api/v1/gpio/PA5` means "make pin PA5 look like this JSON".

### 2.2 Endpoint contract

| Method | Path | Success | Meaning |
|---|---|---|---|
| GET | `/api/v1/system` | 200 | Identity, firmware info, enabled capabilities |
| GET | `/api/v1/system/status` | 200 | Uptime, link state, client/queue counters |
| GET | `/api/v1/gpio` | 200 | Configured pins only, with `count` |
| GET | `/api/v1/gpio/{pin}` | 200 | One configured pin's mode/pull/state |
| PUT | `/api/v1/gpio/{pin}` | 200 or 201 | 201 if newly configured, 200 if only updated |
| DELETE | `/api/v1/gpio/{pin}` | 204 | Deconfigure; no body |
| OPTIONS | any API path | 204 | CORS preflight (needed for port 80 → 8080) |

Canonical names: `PA0`…`PA15`, `PB0`…`PB15`. Parsing them is an **API-layer**
responsibility (they are the wire representation), while deciding whether a pin is
usable stays in the GPIO module.

### 2.3 Layering

```text
┌───────────────────────────────────────────────────────────────────────┐
│ Browser :80  (static site, lwIP HTTPD + fsdata_custom.c)              │
│ Browser :8080 (REST + WS, raw TCP server)      ← cross-origin (CORS)  │
└──────────────────────────────┬────────────────────────────────────────┘
                               ▼
┌───────────────────────────────────────────────────────────────────────┐
│ TRANSPORT     api_server.c : parse HTTP, buffer body, write response  │
│               api_ws.c     : handshake, frames, subscriptions          │
└──────────────────────────────┬────────────────────────────────────────┘
                               ▼
┌───────────────────────────────────────────────────────────────────────┐
│ API CORE      api_router.c : method + path → handler                  │
│               api_json.c   : bounded reader/writer, envelope           │
│               api_errors.c : domain status → HTTP status + JSON code  │
└──────────────────────────────┬────────────────────────────────────────┘
                               ▼
┌───────────────────────────────────────────────────────────────────────┐
│ DOMAIN        events.c  : fixed ring of state-change events            │
│               modules/gpio.c (HAL here)  →  Hardware_Init/Task facade │
└───────────────────────────────────────────────────────────────────────┘
```

Dependency direction is strictly **downwards**. `modules/gpio.c` may include `main.h`
and STM32 HAL today; that is fine. The API must **never** include `main.h`,
`stm32f4xx_hal.h`, or call `HAL_GPIO_*`. If you find HAL in `api_*.c`, the layer boundary
has been broken.

### 2.4 Responsibility of each proposed component

| Component | Owns | Must not do |
|---|---|---|
| `api_server.c` | TCP PCB lifecycle, byte accumulation, `Content-Length`, response framing, CORS, OPTIONS | Know GPIO names or HAL |
| `api_router.c` | Match method + path, call one handler, choose status | Parse raw bytes or JSON |
| `api_json.c` | Bounded parse/emit of flat JSON, envelope | Know HTTP or hardware |
| `api_errors.c` | Map `GPIO_STATUS_*`/validation errors to status + code | Generate JSON directly |
| `events.c` | Ring buffer, subscriber masks, event delivery | Know TCP or WebSockets |
| `api_ws.c` | Handshake, frame codec, ping/pong, subscriptions | Read/write GPIO state |
| `modules/gpio.c` | Pin whitelist, configuration state, HAL calls | Know HTTP/JSON/WebSocket |

Avoid inventing a generic "device framework" with vtables and registries for two
modules. A simple `if/else` chain in the router plus a fixed event struct is enough for an
MCU with 128 KB SRAM.

### 2.5 Shared response model (proposed)

A single small result type keeps handlers uniform:

```c
/* Core/Inc/api_errors.h — PROPOSED sketch */
typedef enum {
    API_OK = 0,
    API_ERR_BAD_REQUEST,      /* 400 INVALID_REQUEST */
    API_ERR_NOT_FOUND,        /* 404 NOT_FOUND / INVALID_PIN / PIN_NOT_CONFIGURED */
    API_ERR_METHOD,           /* 405 METHOD_NOT_ALLOWED */
    API_ERR_CONFLICT,         /* 409 HARDWARE_STATE_CONFLICT */
    API_ERR_TOO_LARGE,        /* 413 BODY_TOO_LARGE */
    API_ERR_CONTENT_TYPE,     /* 415 UNSUPPORTED_MEDIA_TYPE */
    API_ERR_INTERNAL          /* 500 INTERNAL_ERROR */
} api_error_t;
```

Handlers return `api_error_t` plus an HTTP status chosen by the router; the JSON writer
always produces exactly one of:

```json
{ "success": true, "data": { ... } }
{ "success": false, "error": { "code": "...", "message": "..." } }
```

`DELETE` with 204 is the only bodyless response.

### 2.6 One transport or two ports?

For REST alone, a single raw TCP server could serve both static files and JSON on port 80
if you replaced HTTPD entirely — but that means reimplementing static file serving and
losing the working, generated filesystem. The pragmatic split is:

- **port 80**: unchanged lwIP HTTPD for the UI (zero risk to what already works);
- **port 8080**: new raw TCP listener for REST and WebSocket.

Cost: cross-origin requests (Chapter 8 covers CORS and `OPTIONS`). Benefit: the existing
web page stays reachable exactly as today, and API bugs cannot take down static serving.
If you later need same-origin, the alternative is to patch `httpd.c` — a deliberate
submodule modification — or to replace HTTPD with your own static server; neither is
required for the first working API.

Route `API_Init()` from `usb_network_services_init()` so both listeners start together.
Keep the old CGI registration in place temporarily, then remove it once the frontend has
migrated; do not run two competing `/api` implementations indefinitely.

---

## Chapter 3 — Implementing a proper HTTP API server

### 3.1 Raw API rules (read this before writing code)

With `NO_SYS=1` the lwIP raw API callbacks execute **inside the superloop**, ultimately
from `tud_network_recv_cb()` → `netif->input()`. Therefore:

- Callbacks must return quickly. No `while (!data_ready);`, no `HAL_Delay()`.
- Do not call `tud_task()` or `sys_check_timeouts()` from an API callback.
- Never free or retain `pbuf`s incorrectly. If you keep a pointer, either copy the bytes
  into your own buffer or take ownership deliberately.
- Use `tcp_recved()` after consuming received data, or the window will close.
- `tcp_write()` may fail with `ERR_MEM`; buffer the response and retry from `tcp_sent()`.
- Check `tcp_sndbuf()` before writing large responses.
- Set `tcp_arg(pcb, state)` at accept time so every callback finds its connection state.
- Handle `err` (remote reset) and `poll` (idle timeout) callbacks — otherwise PCBs leak.
- Keep exactly one request in flight per connection; close after the response initially.
  Keep-alive can be added later.

### 3.2 Listener skeleton (proposed, intentionally incomplete)

```c
/* Core/Src/api_server.c — PROPOSED skeleton, not a complete implementation */
#include "lwip/tcp.h"
#include "api_server.h"

#define API_PORT        8080
#define API_REQ_BUF     512   /* JSON body limit */
#define API_HDR_BUF     256   /* request line + headers */
#define API_RSP_BUF     1024  /* one response snapshot */

typedef enum { H_REQ, H_HEAD, H_BODY, H_DONE } parse_state_t;

typedef struct {
    struct tcp_pcb *pcb;
    parse_state_t   state;
    char   hdr[API_HDR_BUF];   uint16_t hdr_len;
    char   body[API_REQ_BUF];  uint16_t body_len, content_len;
    char   rsp[API_RSP_BUF];   uint16_t rsp_len, rsp_sent;
    /* method, path, query: pointers into hdr, or small fixed copies */
} api_conn_t;

static err_t api_accept(void *arg, struct tcp_pcb *pcb, err_t err);
static err_t api_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static err_t api_sent(void *arg, struct tcp_pcb *pcb, uint16_t len);
static err_t api_poll(void *arg, struct tcp_pcb *pcb);
static void  api_err(void *arg, err_t err);

void APIServer_Init(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return;
    tcp_bind(pcb, IP_ADDR_ANY, API_PORT);   /* bind-all is simplest here; usb_network.c's
                                               `ipaddr` is static, so binding to the exact
                                               address would first need it exported */
    pcb = tcp_listen(pcb);
    tcp_arg(pcb, NULL);
    tcp_accept(pcb, api_accept);
}
```

`api_accept()` allocates a connection state **from a fixed pool** (e.g. 2 slots),
sets the four callbacks (`tcp_recv`, `tcp_sent`, `tcp_poll`, `tcp_err`), optionally
`tcp_nagle_disable(pcb)`, and returns `ERR_OK`. If the pool is full, close the new PCB
immediately. No `malloc`.

### 3.3 Request parsing as a state machine

`api_recv()` must tolerate any segmentation:

```text
H_REQ  ── find "\r\n" ──► parse "METHOD path HTTP/1.1"
H_HEAD ── find "\r\n\r\n" ──► parse Content-Length, Content-Type, Upgrade, Origin
H_BODY ── received >= Content-Length ──► H_DONE
H_DONE ── dispatch once ──► flush response ──► close
```

Key checks, in this order:

1. **Method**: `GET`, `PUT`, `DELETE`, `OPTIONS`, and the special
   `GET + Upgrade: websocket` case. Anything else → `405` with `Allow: GET, PUT, DELETE, OPTIONS`.
2. **URI**: copy at most e.g. 128 bytes, reject `..`, control characters, or a query on
   API paths (the API uses a body). Split path from query if you support `?`.
3. **Headers**: scan the header buffer line by line with bounded comparisons.
   - `Content-Length:` → parse decimal with overflow guard; `> 512` → **413**.
   - `Transfer-Encoding: chunked` → **400** (we do not support streaming bodies).
   - `Content-Type:` must start with `application/json` for `PUT` → **415** otherwise.
   - `Upgrade: websocket` → hand off to `api_ws.c` **after** the handshake headers.
4. **Body**: accumulate until `body_len == content_len`. A body arriving in three TCP
   segments is the normal case, not an error. Copy with `pbuf_copy_partial(p, ...)` and
   call `tcp_recved(pcb, p->tot_len)` for every pbuf, including ones you drop.
5. **Malformed** → 400 with the JSON error envelope, then close.

Always NUL-terminate within bounds. Never use `strcpy`/`sprintf` on these buffers.

### 3.4 Fragmented requests — why this matters here

NCM carries Ethernet frames of up to ~1500 bytes, and a `PUT` with headers + 200-byte JSON
is typically 350–450 bytes, so it often fits in **one** segment. But:

- the host may split headers across NTBs;
- retransmission and TCP segmentation are outside your control;
- a slow or misbehaving client can send one byte at a time.

If your parser assumes a complete request in the first `tcp_recv()`, it will silently fail
on real hosts. Test this deliberately (Chapter 9) by sending the request in small chunks
with `nc` or Python.

### 3.5 Generating responses

Build the whole small response in `rsp[]`, then send it:

```text
HTTP/1.1 200 OK\r\n
Content-Type: application/json\r\n
Content-Length: <n>\r\n
Access-Control-Allow-Origin: *\r\n
Connection: close\r\n
\r\n
{json body}
```

For 204 send headers only, with `Content-Length: 0`. For 405 include `Allow`.

Send with `tcp_write(pcb, rsp, len, TCP_WRITE_FLAG_COPY)`, then `tcp_output(pcb)`. If
`tcp_write()` returns `ERR_MEM`, remember the unsent offset in `rsp_sent` and retry from
`api_sent()` as window space opens. Close with `tcp_close()` only after all bytes are
acked (or accept that `tcp_close()` queues them and rely on lwIP — verify with a packet
capture; the safe pattern is to close from `api_sent()` when `rsp_sent == rsp_len`).

### 3.6 Status code mapping

| Situation | Status | JSON `error.code` |
|---|---|---|
| Known path, successful read/write | 200 | — |
| New pin configured by PUT | 201 | — |
| Successful DELETE | 204 (no body) | — |
| Bad JSON / missing or wrong-type field / bad pin syntax | 400 | `INVALID_REQUEST` |
| Unknown endpoint | 404 | `NOT_FOUND` |
| Valid pin name but not allowed by module | 404 | `INVALID_PIN` |
| Allowed but unconfigured pin | 404 | `PIN_NOT_CONFIGURED` |
| Unsupported method | 405 | `METHOD_NOT_ALLOWED` |
| State on input pin / mode on unconfigured pin | 409 | `HARDWARE_STATE_CONFLICT` |
| Body > 512 bytes | 413 | `BODY_TOO_LARGE` |
| Missing/wrong `Content-Type` on PUT | 415 | `UNSUPPORTED_MEDIA_TYPE` |
| Unexpected hardware failure | 500 | `INTERNAL_ERROR` |

### 3.7 Connection cleanup

- `api_err()`: free the slot; the PCB is already invalid — do **not** call `tcp_close()`.
- `api_poll()`: count missed polls and `tcp_abort()` after a timeout (e.g. 30–60 s).
- `api_recv(p == NULL)`: peer closed — free the slot.
- Always `tcp_recved()` exactly once per received byte count.

### 3.8 What to test before moving on

- `curl -v http://192.168.7.1:8080/api/v1/system` → 200 + JSON.
- `curl -X POST ...` → 405 + `Allow` header.
- `curl -X PUT ... -H 'Content-Type: application/json' --data '{"x":1}'` on an
  unimplemented path → 404, no crash.
- Send 600 bytes → 413.
- Send headers in two TCP writes → still 200.
- Repeated connect/close 100 times → no PCB leak (watch `MEMP_NUM_TCP_PCB` usage).

---

## Chapter 4 — Implementing JSON

### 4.1 The actual constraint

Bodies are ≤512 bytes, flat objects with at most a handful of fields, no nested arrays in
requests, and the MCU has no external RAM and no `malloc` in our design. That changes the
answer: a full JSON DOM library is overkill.

### 4.2 Options compared

| Option | Pros | Costs |
|---|---|---|
| **cJSON** | Mature, handles escapes/nesting | Needs `malloc`; bring hooks, heap pressure on 128 KB SRAM, ~10s of KB flash; must bound allocation anyway |
| **JSMN** | No allocation, tiny tokenizer | Produces tokens; you still write type/range validation; no object lookup; token array consumes RAM per nesting level |
| **Purpose-built flat parser** (recommended) | ~100–200 lines, zero allocation, exact control over 512-byte limit, easy to audit | Handles only our flat subset; you must reject unsupported syntax explicitly |

A reasonable compromise if you later need nested messages: JSMN with a fixed token array
(e.g. 32 tokens) and no dynamic allocation. But for the first working API, write the small
parser. Do not add a large library "just in case".

### 4.3 Reader design (proposed API)

```c
/* Core/Inc/api_json.h — PROPOSED sketch */
typedef enum { J_OK=0, J_BAD_SYNTAX, J_WRONG_TYPE, J_MISSING, J_OUT_OF_RANGE,
               J_DUPLICATE, J_TOO_MANY } json_result_t;

json_result_t json_get_string(const char *json, const char *key,
                              char *out, size_t out_len);
json_result_t json_get_uint (const char *json, const char *key,
                             unsigned *out, unsigned min, unsigned max);
json_result_t json_get_bool (const char *json, const char *key, unsigned *out);
```

Implementation rules:

- Operate on the NUL-terminated 512-byte body buffer; **never** write into it if it is
  `const`, and copy out string values with an explicit length check.
- Recognize only `"key" : value` with `string`, `unsigned integer`, `true`/`false`,
  and `null`. Reject floats, arrays, objects and `\u` escapes (or support `\"` and `\\`
  only, with a bounded destination).
- Scan keys before values; detect duplicate keys and return `J_DUPLICATE` (ambiguous
  input should be an error, not a coin flip).
- Unknown keys: ignore them but document the choice. Rejecting them makes clients brittle.
- Use `strtoul()` with end-pointer and range checks; reject a leading `+`, whitespace
  noise, and overflow (`errno == ERANGE`).
- Bound the number of members (e.g. 8). Cap string values (e.g. 32 bytes).
- For `PUT /gpio/{pin}`, the router decides which *combination* of `mode`, `pull`, `state`
  is legal — the JSON layer only reports presence and types.

### 4.4 Writer design (proposed API)

```c
/* Core/Inc/api_json.h — PROPOSED sketch */
typedef struct { char *buf; size_t cap, len; } json_writer_t;

void jw_init(json_writer_t *w, char *buf, size_t cap);
int  jw_printf(json_writer_t *w, const char *fmt, ...); /* returns 0 on success */
/* listeners for building arrays incrementally */
```

Rules:

- Every write checks `len + n < cap` **before** writing; truncate/error on overflow.
- Use `snprintf()` into a small temporary when needed, never `sprintf()`.
- Emit fixed strings with `memcpy()` and known lengths.
- For `GET /gpio`, write one `{"pin":"PB0","mode":"output","pull":"none","state":1}`
  item at a time; compute `count` from the module before writing the array, or reserve a
  fixed slot and patch it afterwards. Do not build a second copy of the list.

Envelope helpers [PROPOSED]:

```c
void api_reply_ok   (json_writer_t *w, const char *data_json); /* data = {…} */
void api_reply_error(json_writer_t *w, api_error_t e, const char *code,
                     const char *message);
```

### 4.5 Examples

Request accepted:

```http
PUT /api/v1/gpio/PA5 HTTP/1.1
Host: 192.168.7.1:8080
Content-Type: application/json
Content-Length: 28

{"mode":"input","pull":"up"}
```

Response:

```http
HTTP/1.1 201 Created
Content-Type: application/json
Content-Length: 45
Access-Control-Allow-Origin: *
Connection: close

{"success":true,"data":{"pin":"PA5","configured":true}}
```

Rejected:

```json
{"success":false,"error":{"code":"INVALID_REQUEST","message":"'mode' must be 'input' or 'output'."}}
```

### 4.6 Buffer-overflow checklist

- [ ] Body buffer is exactly 512 + 1 bytes and NUL-terminated.
- [ ] `Content-Length` parsed with overflow guard and compared before copying.
- [ ] Header buffer is sized and checked before every append.
- [ ] `json_get_string()` copies at most `out_len - 1` bytes.
- [ ] Response writer refuses to exceed `API_RSP_BUF`.
- [ ] No `malloc`, no variable-length arrays, no recursion.

---

## Chapter 5 — Implementing GPIO REST endpoints

This is the first complete vertical slice: transport → JSON → router → module → HAL.

### 5.1 What must be added to the GPIO module first

`Core/Inc/modules/gpio.h` **[EXISTING]** has no way to ask "is this pin configured?" or
"what is its mode?" — the API cannot list configured pins without new functions. Add
these **[PROPOSED]** to the *module* (not to `api.c`):

```c
/* additions to Core/Inc/modules/gpio.h — PROPOSED */
int  GPIO_IsPinAllowed(uint8_t port, uint8_t pin);          /* expose existing private check */
int  GPIO_IsConfigured(uint8_t port, uint8_t pin);
GPIO_Status_t GPIO_GetInfo(uint8_t port, uint8_t pin,
                           GPIO_Mode_t *mode, GPIO_Pull_t *pull, uint8_t *state);
GPIO_Status_t GPIO_DeconfigurePin(uint8_t port, uint8_t pin);
```

Implementation notes:

- Store pull state: add `static GPIO_Pull_t pin_pull[2][16];` in `gpio.c`, set it in
  `GPIO_ConfigurePin()`, reset it in `GPIO_Init()`. **[PROPOSED]** — currently missing.
- `GPIO_GetInfo()` should return `INVALID_MODE` if not configured (same convention as
  `GPIO_ReadPin()`), fill `state` via the existing read logic, and never touch HAL for an
  unconfigured pin.
- `GPIO_DeconfigurePin()` should set the pin to a safe state: input + `GPIO_NOPULL` via
  `HAL_GPIO_Init()`, write 0 to the output data register through HAL, then clear
  `pin_configured[port][pin]` and reset cached mode/pull. Do not leave a pin driving after
  "deconfigure".
- Keep `GPIO_IsPinAllowed()` as the **single** whitelist. The API calls it; it never
  copies the PA1–PA8 / PA15 / PB0–PB10 / PB12–PB15 rule.

Naming conversion **[PROPOSED, API layer]**:

```c
/* PA5 -> port=0, pin=5 ; returns 0 on success */
int api_parse_pin(const char *s, uint8_t *port, uint8_t *pin);
/* Accept exactly 'P' + 'A'/'B' + 1..2 digits, value 0..15. Reject everything else. */
```

Do not ask the module to parse strings — module interfaces should stay numeric and HAL-free
from the API's perspective.

### 5.2 `GET /api/v1/gpio`

Algorithm:

1. Iterate `port = 0..1`, `pin = 0..15` **only through** `GPIO_IsConfigured()`.
2. For each configured pin, call `GPIO_GetInfo()`; skip it if the call fails (do not emit
   partial data).
3. Format `{"pin":"PB0","mode":"output","pull":"none","state":1}`.
4. Wrap with `"pins":[...],"count":N`.

```json
{
  "success": true,
  "data": {
    "pins": [
      {"pin":"PB0","mode":"output","pull":"none","state":1},
      {"pin":"PA5","mode":"input","pull":"up","state":0}
    ],
    "count": 2
  }
}
```

Note: `GPIO_Init()` marks **PB0 as configured output** (`gpio.c:64-70`), so a fresh device
always reports at least one pin. That is intentional — do not "fix" it by removing the
seeding, or the LED API breaks.

If `count == 0`, return `"pins":[],"count":0` — still 200.

### 5.3 `GET /api/v1/gpio/{pin}`

1. `api_parse_pin()` → failure: **400** `INVALID_REQUEST` (a syntactically invalid name).
2. `GPIO_IsPinAllowed()` → failure: **404** `INVALID_PIN` (matches the error example in
   the requirements). E.g. `PA9` (USB) or `PC13` (module only supports A/B).
3. `GPIO_IsConfigured()` → failure: **404** `PIN_NOT_CONFIGURED`.
4. `GPIO_GetInfo()` → `ERROR`: **500**.
5. Emit `{"success":true,"data":{"pin":"PA5","mode":"input","pull":"up","state":0}}`.

The 400/404 distinction matters to clients: a typo is fixable client-side, an unavailable
pin is not.

### 5.4 `PUT /api/v1/gpio/{pin}`

Validate the pin name and availability first (same as 5.3), then interpret the body:

| Body fields | Action | Status |
|---|---|---|
| `mode` (+ optional `pull`) | Configure the pin; `pull` defaults to `"none"` | 201 if newly configured, 200 if reconfigured |
| `state` only | Pin must already be configured **as output**, else 409; then `GPIO_WritePin()` | 200 |
| `mode` **and** `state` | Configure, then write; if write fails, report the failure (consider reverting configuration in a future version) | 201/200 |
| neither, or unknown combination | 400 | — |

Pseudo-flow (not full code):

```c
/* PROPOSED — illustrative handler body */
json_result_t jr;
unsigned mode_present, state_present, pull_present, state;
char mode_str[8], pull_str[8];

jr = json_get_string(body, "mode", mode_str, sizeof mode_str);
if (jr == J_MISSING) mode_present = 0;
else if (jr != J_OK) return API_ERR_BAD_REQUEST;
/* ... same for pull, state ... */

if (!mode_present && !state_present) return API_ERR_BAD_REQUEST;

if (mode_present) {
    GPIO_Mode_t m = strcmp(mode_str,"input")==0 ? GPIO_CONFIG_INPUT
                  : strcmp(mode_str,"output")==0 ? GPIO_CONFIG_OUTPUT : INVALID;
    GPIO_Pull_t p = ...;            /* "none"/"up"/"down" */
    if (m == INVALID || p == INVALID) return API_ERR_BAD_REQUEST;
    was_configured = GPIO_IsConfigured(port, pin);
    st = GPIO_ConfigurePin(port, pin, m, p);
    if (st != GPIO_STATUS_OK) return map_gpio_status(st);
}
if (state_present) {
    if (!GPIO_IsConfigured(port, pin)) return API_ERR_CONFLICT;
    st = GPIO_WritePin(port, pin, state ? 1 : 0);
    if (st == GPIO_STATUS_INVALID_MODE) return API_ERR_CONFLICT; /* input pin */
    if (st != GPIO_STATUS_OK) return map_gpio_status(st);
}
Events_Publish(EV_GPIO_CHANGED, pin, mode, state);   /* see 5.6 */
return was_configured ? OK_200 : CREATED_201;
```

Mapping `GPIO_Status_t` **[EXISTING]**:

| HAL/module result | HTTP |
|---|---|
| `GPIO_STATUS_OK` | 200/201 |
| `GPIO_STATUS_INVALID_PIN` | 404 `INVALID_PIN` |
| `GPIO_STATUS_INVALID_MODE` | 409 `HARDWARE_STATE_CONFLICT` |
| `GPIO_STATUS_ERROR` | 500 `INTERNAL_ERROR` |

Do **not** call `GPIO_SetLED()`, `Hardware_GPIO_SetLED()` or `HAL_GPIO_WritePin()` from
the API — even for `PB0`. `GPIO_SetLED()` is a special-purpose helper; the generic path is
`GPIO_WritePin(port, pin, state)`. This is the "keep hardware logic in modules" rule in
practice.

### 5.5 `DELETE /api/v1/gpio/{pin}`

1. Validate name and whitelist as above.
2. If not configured → 404 `PIN_NOT_CONFIGURED` (idempotent DELETE is a valid alternative;
   choose one and document it — 404 is more informative for a hardware UI).
3. `GPIO_DeconfigurePin()` → map errors as in 5.4.
4. Publish a `gpio.changed` event with the deconfigured state.
5. Reply **204** with no body.

Caveat to document in the UI: deconfiguring `PB0` disables the board LED path that
`GPIO_Init()` set up; a subsequent `Hardware_GPIO_SetLED()` call would write to a pin now
configured as input. Either forbid deleting reserved pins or make `GPIO_SetLED()` configure
the pin itself before writing. **[PROPOSED decision needed]**

### 5.6 Events from hardware changes (proposed)

Introduce a tiny event bus instead of putting WebSocket code in the GPIO module or REST
code in the WebSocket handler:

```c
/* Core/Inc/events.h — PROPOSED sketch */
typedef enum { EV_GPIO_CHANGED, EV_SYSTEM_STATUS } event_type_t;

typedef struct {
    event_type_t type;
    uint32_t     seq;          /* monotonic, for clients to de-duplicate */
    union {
        struct { char pin[4]; GPIO_Mode_t mode; GPIO_Pull_t pull; uint8_t state; } gpio;
        struct { uint32_t uptime_ms; } system;
    } data;
} event_t;

void Events_Publish(const event_t *ev);   /* non-blocking: copy into ring or drop */
```

- Fixed ring, e.g. 8 entries, with a `dropped` counter.
- Publish from `GPIO_ConfigurePin`/`GPIO_WritePin`/`GPIO_DeconfigurePin` **only after**
  success, or from the router after the module call succeeds. Pick one place and be
  consistent; publishing from the module makes REST and WebSocket automatically agree.
- Never let a publish block on a slow TCP client. Drop and count.

### 5.7 Chapter checklist

- [ ] New GPIO functions added to `modules/gpio.h` / `gpio.c` and CMake.
- [ ] Pull state tracked.
- [ ] Pin name parser + whitelist call (no duplicated pin table).
- [ ] Four endpoints returning the documented statuses.
- [ ] `Events_Publish()` called on every successful mutation.
- [ ] curl tests for 200/201/204/400/404/409/500.
- [ ] LED still blinks at the documented intervals afterwards.

---

## Chapter 6 — Adding new hardware modules

Use GPIO as the template. The steps are always the same.

### 6.1 The general pattern

1. **Config**: add `#define ENABLE_ADC 1` etc. in `Core/Inc/hardware_config.h`.
2. **Header** `Core/Inc/modules/<name>.h`: types, status enum, `X_Init()`, `X_Task()`,
   configuration/read/deconfigure functions. No HAL types in the API-facing signatures if
   avoidable.
3. **Source** `Core/Src/modules/<name>.c`: HAL-only implementation + private state arrays.
4. **Facade**: extend `Hardware_Init()` / `Hardware_Task()` in `hardware.c` with
   `#if ENABLE_X` guards. Add cross-module helpers (like `Hardware_GPIO_SetLED()`) only
   when genuinely needed.
5. **CMake**: add the new `.c` to `CMakeLists.txt`.
6. **API**: add router handlers, validation, error mapping, event publishing.
7. **Capabilities**: report the module in `GET /api/v1/system` from `ENABLE_*` macros.

Pin ownership is a cross-module concern. Today only `GPIO_IsPinAllowed()` exists and it
does not know about peripherals. Before enabling ADC/PWM/UART/I2C/SPI, add a small
**reservation table** in the hardware layer [PROPOSED], e.g.
`HW_ReservePin(port, pin, owner)` / `HW_IsPinFree()`. Otherwise a client can configure
`PA2` as GPIO while USART2 uses it. This is the most likely real bug once more modules are
enabled.

### 6.2 Module-by-module guide

| Module | Public interface sketch [PROPOSED] | HAL work | Endpoints [PROPOSED] | Validation / events |
|---|---|---|---|---|
| **ADC** | `ADC_Init/Task`, `ADC_Configure(channel, pin)`, `ADC_Read(channel, uint16_t *raw)`, `ADC_Deconfigure(channel)` | `hadc1`, `HAL_ADC_Start/GetValue`, DMA optional | `GET /api/v1/adc`, `GET/PUT/DELETE /api/v1/adc/{channel}` | Channel in range, pin free, sample time; publish `adc.changed` only on configuration (reads are polled, not events, unless you add threshold events) |
| **PWM** | `PWM_Init`, `PWM_Configure(tim, ch, pin, freq_hz)`, `PWM_SetDuty(tim,ch,percent)`, `PWM_Deconfigure` | TIMx PWM mode, `HAL_TIM_PWM_Start` | `GET /api/v1/pwm`, `PUT /api/v1/pwm/{tim}/{ch}`, `DELETE ...` | Frequency bounds vs 96 MHz clock/PSC/ARR, duty 0–100, pin free, no reconfigure while running without stopping; publish `pwm.changed` |
| **UART** | `UART_Init`, `UART_Configure(baud, pins)`, `UART_Send`, `UART_GetStats` | USART2 on PA2/PA3, `HAL_UART_Receive_IT` or DMA into fixed ring | `GET /api/v1/uart`, `PUT /api/v1/uart/0`, `GET /api/v1/uart/0/stats`, `POST`-like via `PUT ... /tx` | Baud list, no concurrent TX, RX buffer overflow counter; publish `uart.rx` sparingly (events can flood) |
| **I2C** | `I2C_Init`, `I2C_Scan`, `I2C_Read(addr,reg,buf,len)`, `I2C_Write(...)` | I2C1, `HAL_I2C_Mem_Read/Write`, timeouts bounded by `sys_now()` | `GET /api/v1/i2c`, `GET /api/v1/i2c/scan`, `PUT /api/v1/i2c/{addr}/reg/{reg}` | 7-bit address range, len ≤ fixed buffer, **never block longer than a few ms** — I2C busy loops are a common superloop killer |
| **SPI** | `SPI_Init`, `SPI_Transfer(tx,rx,len)` | SPI1, `HAL_SPI_TransmitReceive` with bounded len | `PUT /api/v1/spi/0` with bounded hex/base64 payload | len ≤ 64 bytes, no concurrent CS handling in API, bus ownership flag |

There is **no USB CDC** in this project (`CFG_TUD_ECM_RNDIS` is 0, NCM only,
`tusb_config.h:159-160`), so a UART module is a real external UART, not a virtual COM port.

### 6.3 State-change reporting rule

Every module publishes domain events (`EV_ADC_CONFIG_CHANGED`, `EV_PWM_CHANGED`, …) via
`Events_Publish()` after a successful mutation. The transport layer never knows whether an
event came from REST, a timer, or a future local button. This is the single most important
rule for keeping WebSocket and REST consistent.

---

## Chapter 7 — WebSocket implementation

### 7.1 Can lwIP HTTPD do the upgrade?

No — at least not without patching the submodule. `httpd.c` has no `Upgrade` handling and
no callback that hands the `tcp_pcb` to application code. Patching it would mean carrying
a divergent lwIP fork. Since we already have a raw TCP server on **:8080**, handle both
protocols there: after parsing headers, if `Upgrade: websocket` is present, perform the
handshake instead of REST dispatch.

WebSocket from a page served on :80 to :8080 is a **different origin**, but WebSockets
are not governed by CORS fetch rules; the browser sends an `Origin:` header, which the
server may check. REST `fetch()` *is* governed by CORS — see Chapter 8.

### 7.2 Handshake [PROPOSED]

Require:

```http
GET /api/v1/ws HTTP/1.1
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
```

Respond:

```http
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

`Sec-WebSocket-Accept = base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`.
You need a small SHA-1 and base64 implementation (or vendor minimal ones) — a few KB of
flash. Validate the key length (24 base64 chars) and reject anything else with 400. From
then on **do not send HTTP again** on that connection; the same PCB carries frames.

### 7.3 Frames

Client → server frames **must be masked**; server → client frames must not be.

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
|     Extended payload length continued, if len == 127         |
+ - - - - - - - - - - - - - - - +-------------------------------+
|                               |Masking-key, if MASK set      |
+-------------------------------+-------------------------------+
|                     Payload data ...                          |
+---------------------------------------------------------------+
```

Parsing rules for this project:

- Reject unmasked client frames (RFC requirement).
- Reject payloads > 512 bytes (control frames > 125) → close with code 1009.
- Handle 7-bit, 16-bit and 64-bit length fields; for 64-bit, reject the high word if set.
- Unmask in place with the 4-byte key (`data[i] ^= key[i & 3]`).
- Support opcodes: `0x1` text, `0x8` close, `0x9` ping, `0xA` pong, `0x0` continuation.
  Reject binary/reserved opcodes.
- Control frames must not be fragmented; text frames may be, with a fixed assembly buffer.
- Reply to ping with a pong carrying the same payload; ignore unsolicited pong but update
  the last-seen timestamp.

Server frames: build `FIN|opcode`, length (7/16 bit only — we never send >512 bytes),
then payload into a fixed TX buffer, and send via `tcp_write`.

### 7.4 Connection state and limits

Each WS client needs [PROPOSED]:

```c
typedef struct {
    struct tcp_pcb *pcb;
    uint8_t  used;
    uint16_t rx_len, tx_len;
    uint8_t  rx[512], tx[512];
    uint8_t  frag_opcode;
    uint16_t subscriptions;      /* bit0 GPIO, bit1 SYSTEM */
    uint32_t last_seen_ms;
} ws_client_t;
```

Start with **`MAX_WS_CLIENTS = 1`**, maybe 2. Two 512-byte buffers plus state per client is
~1 KB each; with an already busy lwIP heap (`MEM_SIZE≈8.7 KB`) and NCM buffers, more
clients will hurt. Measure with `arm-none-eabi-size` and a temporary `LWIP_STATS` build.

Use `tcp_poll()` for ping keepalive and idle timeout; free on `tcp_err()` / `recv(p==NULL)`.

### 7.5 Subscriptions and messages

Client → server:

```json
{"type":"subscribe","data":{"resource":"gpio"}}
{"type":"ping"}
```

Server → client:

```json
{"type":"gpio.changed","data":{"pin":"PA5","mode":"input","state":1,"seq":17}}
{"type":"pong"}
```

Use the same `api_json.c` reader as REST. On `subscribe`, set a bit in `subscriptions`,
reply with an ack or directly with a fresh snapshot, and remember `seq` if the client
sends `last_seq` so it can request replay from the event ring.

### 7.6 Event sequencing and reconnection

- Every published event gets a monotonic `seq` (Chapter 5.6).
- On connect, the client **first does `GET /api/v1/gpio`** to get a snapshot, then opens the
  socket and subscribes with its last known `seq`. This closes the race where a change
  happens between snapshot and subscription.
- If the ring still holds events with `seq > last_seq`, replay them; otherwise send a
  fresh snapshot so the client can reset.
- Client reconnects with exponential backoff (e.g. 0.5 → 1 → 2 → 5 s).
- Server sends a ping every ~20–30 s and drops clients that miss two intervals; this also
  detects a host that went to sleep without a TCP FIN.

### 7.7 Memory and complexity summary

| Item | Approximate cost | Mitigation |
|---|---|---|
| SHA-1 + base64 for handshake | a few KB flash, ~100 bytes stack | Use a compact implementation; no dynamic buffers |
| Per-client RX/TX buffers | ~1 KB each | Limit to 1–2 clients |
| Event ring (8 × event_t) | ~300–500 bytes | Drop oldest, count drops |
| JSON parse/emit scratch | reuse REST buffers where possible | Never allocate a second 512-byte buffer per client |
| Fragment assembly | reuse RX buffer | Reject >512 bytes early |

WebSocket is the most complex chapter. Do not start it until REST passes all tests.

---

## Chapter 8 — Frontend integration

### 8.1 The current client

`web/js/api.js` **[EXISTING]** uses `fetch('/api?...')` with `URLSearchParams`, and
`web/index.html:66-91` expects `DeviceAPI.setGPIO()` to resolve or throw. It never reads a
response body, so today it cannot distinguish success from the CGI silently ignoring a
bad value.

### 8.2 New API wrapper (proposed)

```javascript
// web/js/api.js — PROPOSED replacement
const API_BASE = "http://192.168.7.1:8080/api/v1";

class ApiError extends Error {
  constructor(status, code, message) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

async function apiFetch(path, options = {}) {
  const response = await fetch(API_BASE + path, {
    ...options,
    headers: { "Content-Type": "application/json", ...(options.headers || {}) }
  });
  if (response.status === 204) return null;
  const body = await response.json();
  if (!response.ok || body.success === false) {
    const e = body.error || {};
    throw new ApiError(response.status, e.code, e.message || response.statusText);
  }
  return body.data;
}

const DeviceAPI = {
  listPins: () => apiFetch("/gpio"),
  getPin:   (pin) => apiFetch("/gpio/" + pin),
  setMode:  (pin, mode, pull) => apiFetch("/gpio/" + pin, {
    method: "PUT", body: JSON.stringify({ mode, pull })
  }),
  setState: (pin, state) => apiFetch("/gpio/" + pin, {
    method: "PUT", body: JSON.stringify({ state })
  }),
  removePin:(pin) => apiFetch("/gpio/" + pin, { method: "DELETE" }),
  system:  () => apiFetch("/system")
};
```

### 8.3 CORS

Because the page is served from `http://192.168.7.1:80` and the API from
`http://192.168.7.1:8080`, the browser treats requests as **cross-origin** (the port is
part of the origin). Your API server must:

- Add `Access-Control-Allow-Origin: *` to every response (or the exact origin).
- Handle `OPTIONS` with 204 and:
  ```text
  Access-Control-Allow-Methods: GET, PUT, DELETE, OPTIONS
  Access-Control-Allow-Headers: Content-Type
  ```
- Without this, `PUT`/`DELETE` fail at preflight before reaching your handler — in the
  browser console you will see a CORS error even though `curl` works. This is the single
  most common "works with curl, fails in browser" cause here.

For WebSocket, check `Origin:` on the handshake if you care; browsers do not apply CORS to
WS, but a permissive server accepts any page.

### 8.4 Live updates without inconsistent UI

Pattern:

1. On load: `const pins = await DeviceAPI.listPins()` → build a **local state map**
   keyed by pin name and render it. This is the authoritative snapshot.
2. Open `new WebSocket("ws://192.168.7.1:8080/api/v1/ws")`.
3. On `open`: send `{"type":"subscribe","data":{"resource":"gpio","last_seq":lastSeq}}`.
4. On message `gpio.changed`: if `event.seq > lastSeq`, update the map and re-render; set
   `lastSeq`.
5. On REST response: also update the map from the returned representation (or re-fetch
   that one pin). Events from your own request will usually arrive too — `seq` prevents
   double/conflicting application.
6. On `close`: set a "disconnected" badge, re-fetch the snapshot, reconnect with backoff,
   and resubscribe with the last `seq`.

```javascript
// web/js/live.js — PROPOSED sketch (keep it small; no framework on this MCU)
let lastSeq = 0;
const pins = new Map();

function render() { /* build the pin list from pins Map */ }

async function refreshSnapshot() {
  const list = await DeviceAPI.listPins();
  pins.clear();
  list.pins.forEach(p => pins.set(p.pin, p));
  render();
}

function connectWS() {
  const ws = new WebSocket("ws://192.168.7.1:8080/api/v1/ws");
  ws.onopen = () => ws.send(JSON.stringify({
    type: "subscribe",
    data: { resource: "gpio", last_seq: lastSeq }
  }));
  ws.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg.type === "gpio.changed" && msg.data.seq > lastSeq) {
      lastSeq = msg.data.seq;
      pins.set(msg.data.pin, msg.data);
      render();
    } else if (msg.type === "pong") {
      /* last pong seen — keepalive */
    }
  };
  ws.onclose = () => {
    renderDisconnected();
    refreshSnapshot().catch(() => {});
    setTimeout(connectWS, backoffNext());
  };
}
```

Do not apply an event with an older `seq` — out-of-order delivery after reconnection is
normal. Do not render optimistically from form values and then let an older event overwrite
them; the server response/event is authoritative.

---

## Chapter 9 — Testing and debugging

### 9.1 Baseline (do this before any code changes)

1. Build: `cmake --preset Debug && cmake --build --preset Debug` (README.md:44-49).
   This project's presets build into `build/Debug/`, not `build/`.
2. Flash via ST-Link (README.md:51-60) — ST-Link V2 is for programming/debug; the USB
   cable is the network interface. Keep both connected. This was executed successfully
   once on the reference board; see Appendix C.
3. Host gets `192.168.7.2/3/4` via DHCP. `ping 192.168.7.1`.
4. `curl -i http://192.168.7.1/` → `HTTP/1.0 200 OK`, `Content-Type: text/html`,
   `Content-Length: 3142` (the generated `index.html`).
5. `curl -i 'http://192.168.7.1/api?device=gpio&port=1&pin=0&state=1'` → **the same
   `200 OK` + `text/html` + 3142-byte body**. This is the CGI returning a *filename* to
   serve, not a JSON response and not a redirect — see Appendix C. PB0 toggles.
6. Also confirm the current failure modes so the "after" state is unambiguous:
   `curl -i http://192.168.7.1/nope`, `curl -i -X PUT http://192.168.7.1/api`, and
   `curl -i http://192.168.7.1/api/v1/gpio` all report **`curl: (52) Empty reply from
   server`** — today there is no 404, no 405 and no JSON.
7. Watch `PC13`: 1 s blink = mounted. If it blinks fast, USB enumeration failed — the API
   cannot work yet.

Expected baseline (verified on the reference board):

```text
$ curl -i http://192.168.7.1/api?device=gpio&port=1&pin=0&state=1
HTTP/1.0 200 OK
Server: lwIP/2.1.2 (http://savannah.nongnu.org/projects/lwip)
Content-Length: 3142
Content-Type: text/html

<!DOCTYPE html> ...
```

### 9.2 REST tests (once :8080 exists)

```bash
# 200 — system info
curl -i http://192.168.7.1:8080/api/v1/system

# 200 — list (PB0 should be present from GPIO_Init)
curl -i http://192.168.7.1:8080/api/v1/gpio

# 404 — allowed but unconfigured (PA5 before configuration)
curl -i http://192.168.7.1:8080/api/v1/gpio/PA5

# 201 — configure
curl -i -X PUT http://192.168.7.1:8080/api/v1/gpio/PA5 \
  -H 'Content-Type: application/json' \
  --data '{"mode":"input","pull":"up"}'

# 200 — read back
curl -i http://192.168.7.1:8080/api/v1/gpio/PA5

# 409 — state on an input pin
curl -i -X PUT http://192.168.7.1:8080/api/v1/gpio/PA5 \
  -H 'Content-Type: application/json' --data '{"state":1}'

# 201 then 200 — output pin, set state twice
curl -i -X PUT http://192.168.7.1:8080/api/v1/gpio/PB0 \
  -H 'Content-Type: application/json' --data '{"mode":"output","pull":"none"}'
curl -i -X PUT http://192.168.7.1:8080/api/v1/gpio/PB0 \
  -H 'Content-Type: application/json' --data '{"state":1}'

# 204 — deconfigure (note the LED caveat from 5.5)
curl -i -X DELETE http://192.168.7.1:8080/api/v1/gpio/PB0

# 405 — method not allowed
curl -i -X POST http://192.168.7.1:8080/api/v1/gpio

# 404 — unknown endpoint / unavailable pin
curl -i http://192.168.7.1:8080/api/v1/nope
curl -i http://192.168.7.1:8080/api/v1/gpio/PA9

# 400 — bad syntax / bad JSON / wrong type
curl -i http://192.168.7.1:8080/api/v1/gpio/PC13
curl -i -X PUT http://192.168.7.1:8080/api/v1/gpio/PA5 \
  -H 'Content-Type: application/json' --data '{broken'

# 415 — wrong content type
curl -i -X PUT http://192.168.7.1:8080/api/v1/gpio/PA5 \
  -H 'Content-Type: text/plain' --data '{"mode":"input"}'

# 413 — oversized body
python3 -c 'print("{"+"\"x\":"+"1"*600+"}")' > /tmp/big.json
curl -i -X PUT http://192.168.7.1:8080/api/v1/gpio/PA5 \
  -H 'Content-Type: application/json' --data-binary @/tmp/big.json
```

Inspect with `curl -i` (status + headers) and DevTools → Network → Headers/Response.
Check that `Content-Type: application/json` and the envelope are present, and that 204
has no body.

### 9.3 Fragmented TCP test

```python
import socket, time
req = ("PUT /api/v1/gpio/PA5 HTTP/1.1\r\n"
       "Host: 192.168.7.1:8080\r\n"
       "Content-Type: application/json\r\n"
       "Content-Length: 28\r\n"
       "Connection: close\r\n\r\n"
       '{"mode":"input","pull":"up"}')
s = socket.create_connection(("192.168.7.1", 8080))
for part in (req[:20], req[20:60], req[60:120], req[120:]):
    s.sendall(part.encode()); time.sleep(0.2)
print(s.recv(4096).decode(errors="replace"))
s.close()
```

Expected: one 201, not four 400s. Also try sending the final body byte separately.

### 9.4 WebSocket tests

- Browser Console:
  ```javascript
  const ws = new WebSocket("ws://192.168.7.1:8080/api/v1/ws");
  ws.onopen = () => ws.send(JSON.stringify({type:"subscribe",data:{resource:"gpio"}}));
  ws.onmessage = e => console.log(e.data);
  ```
  Then issue a `curl` PUT from the shell — a `gpio.changed` event with a `seq` should
  appear in the browser console.
- `websocat -v ws://192.168.7.1:8080/api/v1/ws` (or `wscat`) for a CLI client.
- Send a masked ping frame from a Python script and verify a pong with identical payload.
- Kill the host Wi-Fi/sleep the host: server should time out via `tcp_poll()` and free the
  client slot; on wake the JS should reconnect and re-fetch the snapshot.

### 9.5 Debugging on the device

- No printf today: `__io_putchar()` is weak/unimplemented (`Core/Src/syscalls.c:80-90`).
  Add a strong implementation in a **new** source file (e.g. ITM at SWO pin) if you need
  traces; avoid editing CubeMX files.
- Use the `PC13` blink pattern to detect a stuck superloop. A stalled API callback stops
  `LED_Status_Task()`, so the LED freezes — an immediate signal.
- To measure memory: build temporarily with `-DLWIP_STATS=1`, expose lwIP stats through
  `/api/v1/system/status`, and compare `mem` / `MEMP_TCP_PCB` / `MEMP_PBUF_POOL` usage
  before and after the API. Use `arm-none-eabi-size build/usb-webserver.elf` for flash
  and static RAM deltas, and inspect the `.map` for the biggest objects.
- Use Wireshark on the host USB-NCM interface to confirm whether a failure is TCP-level
  (retransmissions, zero window) or application-level (wrong response bytes).

### 9.6 Failure matrix to verify

| Test | Expected |
|---|---|
| Unsupported method | 405 + `Allow`, no crash |
| Unknown endpoint | 404 JSON envelope |
| Invalid pin syntax / unavailable pin / unconfigured pin | 400 / 404 `INVALID_PIN` / 404 `PIN_NOT_CONFIGURED` |
| Wrong type, missing field, duplicate key | 400 |
| Body 513+ bytes | 413, rest of body drained/connection closed |
| Wrong `Content-Type` | 415 |
| Header split across TCP segments | still one valid response |
| Peer resets mid-request | `api_err()` frees slot, device keeps serving |
| 100 connect/close cycles | PCB/pool count returns to baseline |

---

## Chapter 10 — Implementation roadmap

Do these in order. Each step ends with a working, testable state. Do not jump ahead.

### Step 0 — Confirm the baseline

- **Files**: none changed.
- **Do**: build, flash, ping, `curl` the static page and the existing CGI; watch PC13.
- **Test**: LED 1 s blink, `192.168.7.2`, HTML over HTTP.
- **Success**: you can reproduce the current behaviour at will.
- **Mistakes**: debugging the API before the USB network is up; forgetting that HTML edits
  require a rebuild of `fsdata_custom.c`.

### Step 1 — Raw TCP listener + one GET

- **Create**: `Core/Inc/api_server.h`, `Core/Src/api_server.c`.
- **Modify**: `Core/Src/api.c` → call `APIServer_Init()` from `API_Init()`;
  `CMakeLists.txt` → add `api_server.c`.
- **Implement**: `tcp_new/bind/listen/accept`, fixed pool of 2 connections, request-line
  parse, `GET /api/v1/system` returning a minimal JSON object with 200.
- **Test**: `curl -i http://192.168.7.1:8080/api/v1/system`.
- **Success**: first JSON response from the device; no change to port 80.
- **Mistakes**: blocking in `tcp_recv()`; forgetting `tcp_recved()`; not handling `p == NULL`;
  forgetting to add the file to CMake.

### Step 2 — Status codes and envelope

- **Create**: `api_errors.h/.c`, `api_json.h/.c` (writer first).
- **Implement**: 404 unknown path, 405 method with `Allow`, consistent
  `{"success":...,"data"/"error":...}`, `HEAD` not yet supported.
- **Test**: curl 404/405; check headers and JSON shape.
- **Success**: every response has the documented envelope and status.
- **Mistakes**: using `sprintf`; emitting HTML error pages.

### Step 3 — Body reading and JSON parsing

- **Modify**: `api_server.c` (header/body state machine), `api_json.c` (reader).
- **Implement**: `Content-Length`, 512-byte cap, `Content-Type` check, fragmented body,
  `json_get_string/uint/bool`, duplicate-key and type errors.
- **Test**: Chapter 9.2/9.3 malformed + fragmented cases.
- **Success**: oversized → 413, wrong type → 415, split headers still work.
- **Mistakes**: assuming one pbuf per request; off-by-one in buffer termination.

### Step 4 — Extend the GPIO module

- **Modify**: `Core/Inc/modules/gpio.h`, `Core/Src/modules/gpio.c`.
- **Implement**: public `GPIO_IsPinAllowed`, `GPIO_IsConfigured`, `GPIO_GetInfo`,
  `GPIO_DeconfigurePin`, `pin_pull[]`.
- **Test**: a temporary debug endpoint or on-device assert that lists configured pins.
- **Success**: PB0 reported as configured output; toggling it still changes the LED.
- **Mistakes**: duplicating the whitelist in the API; clearing PB0's configured flag during
  `GPIO_Init()`.

### Step 5 — GPIO GET endpoints

- **Create**: `api_router.h/.c`.
- **Implement**: pin-name parser, `GET /gpio`, `GET /gpio/{pin}` with 400/404/500 mapping.
- **Test**: curl the exact examples in Chapter 5.2/5.3.
- **Success**: JSON matches the example shape, `count` reflects configured pins only.
- **Mistakes**: listing all 32 physical pins; returning 500 for an unavailable pin.

### Step 6 — PUT, DELETE and events

- **Create**: `events.h/.c`.
- **Modify**: router + GPIO module to publish on successful mutation.
- **Implement**: Chapter 5.4/5.5, 201/200/204/409 mapping, reserve-pin decision for PB0.
- **Test**: full matrix from Chapter 9.6; LED still blinks.
- **Success**: REST is the authoritative way to change GPIO; events carry `seq`.
- **Mistakes**: calling HAL from the router; publishing an event before the hardware call
  succeeds; forgetting that DELETE on PB0 changes LED behaviour.

### Step 7 — Frontend migration and CORS

- **Modify**: `web/js/api.js`, `web/index.html`, optionally add `web/js/live.js`;
  API server for `OPTIONS` + CORS headers.
- **Implement**: `fetch()`-based client, state map rendering, error display using
  `error.code`.
- **Test**: DevTools Network tab — confirm preflight 204 and successful PUT; verify the
  page works after a rebuild of `fsdata_custom.c`.
- **Success**: the UI configures a pin and reports the server's error codes.
- **Mistakes**: testing only with curl (no CORS preflight); forgetting to rebuild the
  generated filesystem.

### Step 8 — Robustness pass

- **Implement**: overflow guards, `tcp_poll` timeouts, event `dropped` counter exposed in
  `/system/status`, memory measurements.
- **Test**: 100 connect/close cycles, fragmented/malformed requests, `arm-none-eabi-size`
  before/after.
- **Success**: no leaks, no crashes, known RAM/flash budget.
- **Mistakes**: adding keep-alive before the simple close-after-response path is stable.

### Step 9 — WebSocket handshake, frames, ping/pong

- **Create**: `api_ws.h/.c`; add SHA-1/base64 source.
- **Modify**: `api_server.c` to detect `Upgrade:` and hand off; `CMakeLists.txt`.
- **Implement**: 101 handshake, masked frame parsing, text/ping/pong/close, 1 client.
- **Test**: browser console or `websocat`; ping/pong; invalid key → 400.
- **Success**: a client can connect, ping and receive pong; REST still works.
- **Mistakes**: sending HTTP after 101; accepting unmasked frames; unbounded frame length.

### Step 10 — Subscriptions, events, reconnection

- **Modify**: `api_ws.c`, `events.c`, `web/js/live.js`.
- **Implement**: subscription masks, event replay by `seq`, frontend snapshot + resubscribe.
- **Test**: change GPIO via curl, observe event in browser; disconnect/reconnect during a
  change and verify `seq` de-duplication.
- **Success**: UI state always converges to the device state.
- **Mistakes**: generating `gpio.changed` inside the WebSocket handler independently of
  the actual hardware call; letting a slow client block `Events_Publish()`.

### Final acceptance checklist

- [ ] Port 80 serves the unchanged generated static site.
- [ ] Port 8080 serves `/api/v1/*` with correct statuses, envelope and CORS.
- [ ] No HAL calls in `api_*.c`; no HTTP knowledge in `modules/*.c`.
- [ ] No `malloc`, no blocking waits, no lwIP submodule changes.
- [ ] GPIO events are published from successful hardware mutations, with `seq`.
- [ ] Frontend fetches a snapshot, subscribes, and reconnects safely.
- [ ] Flash/RAM measured and acceptable on the STM32F411.

---

## Appendix A — Do / Don't quick reference

| Do | Don't |
|---|---|
| Keep lwIP HTTPD on :80 for static files | Patch `lib/lwip/**` to add REST methods |
| Use a raw TCP listener on :8080 | Call `HAL_GPIO_*` from `api_*.c` |
| Use fixed buffers with explicit limits | `malloc` / `sprintf` / VLA |
| Validate pin names in API, whitelist in module | Duplicate the PA/PB whitelist in the API |
| Map `GPIO_Status_t` to documented HTTP codes | Silently ignore a failed hardware call |
| Publish events after successful mutations | Generate events independently per transport |
| Fetch snapshot → subscribe → apply `seq` | Trust WebSocket alone for initial UI state |
| Add CORS + `OPTIONS` for :80 → :8080 | Assume curl success means browser success |
| Add new module `.c` files to `CMakeLists.txt` | Edit `Core/Src/gpio.c` (CubeMX output) |
| Verify with ping + PC13 blink first | Debug the API before USB networking works |

## Appendix B — Where things currently live

| Concern | File(s) |
|---|---|
| Superloop, USB init, LED blink | `Core/Src/main.c` |
| NCM/lwIP glue, netif, DHCP config | `Core/Src/usb_network.c` |
| DHCP server implementation | `Core/Src/dhserver.c` |
| lwIP options (HTTPD, TCP, memory) | `Core/Inc/lwipopts.h` |
| TinyUSB/NCM options | `Core/Inc/tusb_config.h` |
| Current CGI proof of concept | `Core/Src/api.c`, `Core/Inc/api.h` |
| API transport (TCP + HTTP framing) | `Core/Src/api_server.c`, `Core/Inc/api_server.h` |
| API routing + pin-name parsing | `Core/Src/api_router.c`, `Core/Inc/api_router.h` |
| Bounded JSON writer | `Core/Src/api_json.c`, `Core/Inc/api_json.h` |
| Hardware module dispatcher | `Core/Src/hardware.c`, `Core/Inc/hardware_config.h` |
| Application GPIO implementation | `Core/Src/modules/gpio.c`, `Core/Inc/modules/gpio.h` |
| CubeMX GPIO init (do not edit) | `Core/Src/gpio.c`, `Core/Inc/gpio.h`, `usb-webserver.ioc` |
| Embedded filesystem generation | `CMakeLists.txt`, `fsdata_custom.c`, `web/**` |
| Frontend API helper | `web/js/api.js` |
| HTTPD request parsing / CGI dispatch | `lib/lwip/src/apps/http/httpd.c` (read-only) |

## Appendix C — Verified baseline log

This was captured by building the current sources, flashing the STM32F411CEU6 over
ST-Link (SWD, 4 MHz), and driving the USB-NCM interface from the host. It is the ground
truth the rest of this tutorial is calibrated against.

Toolchain / flashing:

```text
arm-none-eabi-gcc 14.3.1 (GNU Tools for STM32 14.3.rel1)
cmake --preset Debug && cmake --build --preset Debug   # output: build/Debug/usb-webserver.elf
STM32_Programmer_CLI -c port=SWD freq=4000 -w build/Debug/usb-webserver.elf -v -rst
  Device name : STM32F411xC/E   NVM size: 512 KBytes
  File size   : 109.43 KB @ 0x08000000   (download verified, reset performed)
```

Host network (Linux):

```text
enp0s20f0u1: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500
    link/ether 02:02:84:6a:96:00            # matches tud_network_mac_address
    inet 192.168.7.2/24 brd 192.168.7.255 scope global dynamic
    valid_lft 86351sec                       # ~24 h DHCP lease from dhserver.c
192.168.7.0/24 dev enp0s20f0u1 proto kernel scope link src 192.168.7.2
```

Successful baseline:

```text
$ ping -c3 192.168.7.1
64 bytes from 192.168.7.1: icmp_seq=1 ttl=255 time=3.37 ms
3 packets transmitted, 3 received, 0% packet loss

$ curl -i http://192.168.7.1/
HTTP/1.0 200 OK
Server: lwIP/2.1.2 (http://savannah.nongnu.org/projects/lwip)
Content-Length: 3142
Content-Type: text/html

<!DOCTYPE html> ...   (the generated index.html)

$ curl -i 'http://192.168.7.1/api?device=gpio&port=1&pin=0&state=1'
HTTP/1.0 200 OK
Server: lwIP/2.1.2 (http://savannah.nongnu.org/projects/lwip)
Content-Length: 3142
Content-Type: text/html

<!DOCTYPE html> ...   (SAME body: the CGI returned "/index.html" as a filename)
```

Limitations confirmed at runtime (all return an *empty reply*, i.e. connection closed with
no HTTP response, because `LWIP_HTTPD_SUPPORT_EXTSTATUS=0` and `LWIP_HTTPD_DYNAMIC_HEADERS=0`):

```text
$ curl -i http://192.168.7.1/api/v1/gpio        # target REST path: not implemented
curl: (52) Empty reply from server

$ curl -i -X PUT http://192.168.7.1/api         # unsupported method
curl: (52) Empty reply from server

$ curl -i -X DELETE http://192.168.7.1/api      # unsupported method
curl: (52) Empty reply from server

$ curl -i http://192.168.7.1/nope               # unknown path, no /404.html present
curl: (52) Empty reply from server

$ curl -i 'http://192.168.7.1/api?device=gpio&port=0&pin=9&mode=output'
HTTP/1.0 200 OK
Content-Length: 3142
Content-Type: text/html
    # PA9 is rejected by GPIO_IsPinAllowed(), but the handler ignores the status and
    # still returns "/index.html": the client sees success for a failed hardware call.
```

The device stays responsive (ping succeeds before and after the empty-reply cases), so
these are protocol-level behavior, not crashes. This is exactly the gap the new API on
port 8080 replaces: real status codes, a JSON envelope, and hardware errors surfaced to
the client.

Known static files served correctly: `/`, `/index.html` (3142 B), `/css/style.css`
(6580 B, `text/css`). Edits to `web/**` require a rebuild because the bytes are embedded
in `fsdata_custom.c`.

Firmware footprint of the baseline (`Debug`, i.e. `-O0`; a `Release` build will be
smaller). This is the budget the new API must fit into:

```text
$ arm-none-eabi-size build/Debug/usb-webserver.elf
   text    data     bss     dec     hex
 111732     316   41020  153068   255ec

$ arm-none-eabi-size -A build/Debug/usb-webserver.elf   # (key sections)
.text    88736
.rodata  22572
.data      316
.bss     39480
._user_heap_stack 1540
```

So the baseline already uses roughly 112 KB of the 512 KB flash and ~41 KB of the 128 KB
SRAM before any new API buffers. Plan for the API + WebSocket additions (a few KB) with
that headroom in mind, and re-measure after each roadmap step.

---

## Appendix D — Verified increment log: read-only router

This records roadmap **Step 1** (transport) plus **Steps 4–5** (GPIO metadata and the
read-only `GET` endpoints), captured on the real board. Unlike Chapters 3–5, everything
below is implemented and was tested over the wire.

### D.1 What was built

| File | Responsibility | Notes |
|---|---|---|
| `Core/Src/api_server.c` | TCP lifecycle + HTTP framing only | Fixed 2-slot pool; no GPIO/HAL knowledge |
| `Core/Src/api_router.c` | Route table + pin-name parsing | Calls `GPIO_IsPinAllowed()` / `GPIO_GetPinInfo()` |
| `Core/Src/api_json.c` | Bounded JSON writer | Appends into a caller buffer; latches an overflow flag |
| `Core/Inc/modules/gpio.h` | Pin metadata authority | Public `GPIO_IsPinAllowed()`, `GPIO_GetPinInfo()` |

Boundaries held: `api_server.c` includes neither `modules/gpio.h` nor HAL; `api_router.c`
includes no HAL; the whitelist lives only in `modules/gpio.c`.

Buffer budget per connection slot: `req` 513 B, `body` 2048 B (router output), `rsp`
2304 B (framed message). Two slots, all static.

### D.2 Realized contract

```text
GET  /api/v1/system          -> 200  {"success":true,"data":{...firmware...}}
GET  /api/v1/gpio            -> 200  every allowed pin, each with "configured"
GET  /api/v1/gpio/{pin}      -> 200  one pin; {pin,configured[,mode,pull,value]}
                             -> 404  syntactically valid but not whitelisted
                             -> 400  malformed pin name
                             -> 405  non-GET, with "Allow: GET"
                             -> 500  defensive (JSON buffer overflow)
```

`mode`, `pull`, and `value` are emitted **only** when `configured` is true, so an
unconfigured pin never masquerades as an input with pull-none and value 0. `value` is the
sampled pad level (`IDR`), not the output latch.

### D.3 Test transcript (host `192.168.7.2`, device `192.168.7.1`)

List (871 B; PB0 configured from `GPIO_Init()`):

```text
$ curl -s http://192.168.7.1:8080/api/v1/gpio
{"success":true,"data":{"pins":[{"pin":"PA1","configured":false}, ...,
{"pin":"PB0","configured":true,"mode":"output","pull":"none","value":0},
{"pin":"PB1","configured":false}, ... {"pin":"PB15","configured":false}]}}
```

Error contract:

```text
GET /api/v1/gpio/PA0   -> 404 {"success":false,"error":{"code":"PIN_NOT_FOUND",...}}
GET /api/v1/gpio/ZZZ   -> 400 {"success":false,"error":{"code":"INVALID_PIN",...}}
GET /api/v1/gpio/PA16  -> 400 {"success":false,"error":{"code":"INVALID_PIN",...}}
GET /api/v1/gpio/PC5   -> 400 {"success":false,"error":{"code":"INVALID_PIN",...}}
GET /api/v1/gpio/pa5   -> 400 {"success":false,"error":{"code":"INVALID_PIN",...}}
GET /api/v1/gpio/      -> 404 {"success":false,"error":{"code":"NOT_FOUND",...}}
POST /api/v1/gpio      -> 405 + "Allow: GET"
```

Transport invariants re-checked after the refactor:

- A header split across three TCP segments still yields `200` (fragmented test).
- 20 sequential requests: 20 × `200`/`404`, no failures.
- 3 simultaneous connections: two served, the third reset by the 2-slot limit; the pool
  recovers immediately afterwards.
- Legacy lwIP HTTPD on port 80 still serves `/` (`3142` bytes).

### D.4 Footprint after the increment

```text
RAM:   51136 B / 128 KB (39.01%)   # was 43712 B (33.35%)
FLASH: 120040 B / 512 KB (22.90%)  # was 116688 B (22.26%)
```

The +7.4 KB RAM is the second connection's `body`+`rsp` buffers plus the enlarged
response path; flash grows by the router, writer, and JSON bodies.

### D.5 Deviations from the proposed design

- No separate `api_errors.h/.c`: generic status→body mapping is small enough to stay
  split between the transport (framing errors) and the router (routing errors).
- `GET /api/v1/system` moved from the transport into the router, so all `/api/v1/*`
  routing lives in one place; `API_FIRMWARE_VERSION` moved to `Core/Inc/api.h`.
- The transport body buffer is 2048 B, not "~512 B": the full pin list is ~870 B today
  and would reach ~1.8 KB if every allowed pin were configured.

### D.6 Still to come

Mutation endpoints (`PUT /api/v1/gpio/{pin}`, `PUT /api/v1/gpio/{pin}/value`) stay out of
this increment so the routing and serialization can be reviewed independently. They must
first add explicit `Content-Length` / `Transfer-Encoding` framing and a bounded body
parser, then validate JSON before any hardware call, and let `modules/gpio.c` remain the
authority on whether a pin may be configured or written.

---

## Appendix E — Verified increment log: request framing

Roadmap **Step 3** (body reading), done *before* any mutation route. The HTTP boundary is
now correct on its own, so the hardware-changing endpoints can be added against a
transport that already refuses ambiguous framing.

### E.1 Framing rules

A request is dispatched only when it is complete:

- Header block terminated by `CRLFCRLF`, accumulated one byte at a time into `req[512]`.
- Then exactly `Content-Length` body bytes, accumulated into `req_body[512]`.
- Anything else is a framing error, answered before dispatch.

| Condition | Result |
|---|---|
| `Content-Length` absent | body length 0, dispatch immediately |
| `Content-Length` = 0 | dispatch immediately |
| `Content-Length` valid, ≤ 512 | wait for that many bytes, then dispatch |
| `Content-Length` > 512 | `413 REQUEST_TOO_LARGE` (no body byte buffered) |
| `Content-Length` non-numeric, or empty, or duplicated | `400 INVALID_CONTENT_LENGTH` |
| `Transfer-Encoding` present (any value) | `400 UNSUPPORTED_TRANSFER_ENCODING` |
| header block exceeds 512 B | `413 REQUEST_TOO_LARGE` |

`Transfer-Encoding` is refused rather than decoded: the status set for this API is
`200/201/204/400/404/405/409/413/415/500`, so RFC 7230's "501 for an unsupported transfer
coding" is folded into `400` with a dedicated error code. Guessing framing would risk a
request/response boundary mismatch, which is worse than an explicit rejection.

### E.2 States

```text
API_CONN_IDLE -> API_CONN_HEADERS -> API_CONN_BODY -> API_CONN_SEND
                     |                    |
                     +-- no body ---------+--> dispatch (-> SEND)
                     +-- framing error ----------> respond (-> SEND)
```

`api_recv()` walks each TCP segment with an offset. Headers are copied byte-by-byte so no
body byte is ever written into the header buffer; when the header block ends mid-segment
the loop switches to body accumulation and consumes the rest of that same segment. Both
the header and the body phases count against the existing 10 s idle / 30 s lifetime
watchdog, so a stalled body cannot pin a slot.

### E.3 HEAD

The router resolves `HEAD` exactly like `GET` (same route, same status), and the transport
sets `Content-Length` to the entity length but sends no body. `Allow` is now
`GET, HEAD`. This means a `HEAD` on a valid route returns the same metadata a `GET` would,
which is what an efficient client wants to probe with.

### E.4 Test transcript

Regression (unchanged read-only surface):

```text
GET /api/v1/system     -> 200, 85 B
GET /api/v1/gpio       -> 200, 871 B
GET /api/v1/gpio/PA5   -> 200
GET /api/v1/gpio/PB0   -> 200 (configured output)
GET /api/v1/gpio/PA0   -> 404 PIN_NOT_FOUND
GET /api/v1/gpio/ZZZ   -> 400 INVALID_PIN
POST /api/v1/gpio      -> 405 + "Allow: GET, HEAD"
```

Framing (socket client, checking for bytes before/after the body):

```text
T1a no response before body              PASS (0 bytes during the 1 s gap)
T1b response after body                  PASS (200)
T2  one segment header+body              PASS (200)
T3a no response mid-body (3 pieces)      PASS (0 bytes)
T3b response after last piece            PASS (200)
T4a 512-byte body accepted               PASS (200)
T4b 513-byte body rejected               PASS (413)
T5  incomplete body: no HTTP response     PASS (empty)
T5  connection closed by watchdog         PASS (closed after 10.0 s)
```

Also re-checked: a header block split mid-request-line still returns 200; 20 sequential
requests all answered; 30 connect/close cycles with no failures; the legacy lwIP HTTPD on
port 80 still serves `/` (3142 B).

HEAD on the wire (headers only, no JSON after the blank line):

```text
HTTP/1.1 200 OK
Content-Type: application/json
Content-Length: 871     <- equals GET /api/v1/gpio
Connection: close
<CRLFCRLF>              <- nothing follows
```

### E.5 Footprint

```text
RAM:   52184 B / 128 KB (39.81%)   # was 51136 B (39.01%)
FLASH: 121372 B / 512 KB (23.15%)  # was 120040 B (22.90%)
```

The +1 KB RAM is the two 512-byte request-body buffers (one per slot). Everything is
still static.

### E.6 Known edges for the mutation stage

- `Expect: 100-continue` is currently ignored. A client that waits for `100 Continue`
  before sending its body will stall until the watchdog closes it. Handle it before PUT.
- A `HEAD` whose header block is oversized is answered with `413` *including* a body,
  because the method is not yet parsed at that point. Cosmetic, not a framing risk.
- `Content-Type` is not inspected yet. It becomes a `415` gate for `PUT` bodies in
  Stage 2, before JSON is parsed.
- The parsed request body (`req_body` / `req_body_len`) is now available to the router but
  unused by the read-only routes. Stage 2 extends the router signature to pass it through.

### E.7 Stage 2 — the mutation invariant

GPIO mutations (`PUT /api/v1/gpio/{pin}` and `PUT /api/v1/gpio/{pin}/value`) must hold
this line:

> No hardware function is called until the entire request has been received, its framing
> validated, its JSON parsed, and every required field and value validated.

That is now structurally enforceable: by the time a route runs, the full body is in
`req_body` and the framing has been accepted. Stage 2 adds the `Content-Type` gate, the
bounded JSON parse, field validation, and only then the call into `modules/gpio.c`.

---

## Appendix F — Verified increment log: GPIO mutations

Roadmap **Step 6** (mutation endpoints), built in two reviewable halves. The framing +
validation half was reviewed and approved before the second half added the two GPIO calls,
so the mutation boundary was independently verifiable before it could touch a pin.

### F.1 What changed

| File | Change |
|---|---|
| `Core/Src/api_server.c` | Captures `Content-Type` (duplicate → 400 §F.2.2), rejects any `Expect` header, tracks `has_content_length`; passes an `API_Request_t` to the router |
| `Core/Inc/api_router.h` | Router now receives `{body, body_len, body_present, content_type}`; `path` may be split in place |
| `Core/Src/api_router.c` | Route-shape resolution with per-route `Allow`; the two mutation handlers and the validation chain; `apply_configuration()` / `apply_value()` |
| `Core/Inc/api_json.h`, `Core/Src/api_json.c` | Bounded JSON **reader** added next to the writer |
| `Core/Src/modules/gpio.c` | Unchanged — still the authority; tracking arrays written only after `HAL_GPIO_Init()` |

The router is the only layer that touches the GPIO module, and only through the two
`apply_*()` functions at the very end of the validation chain. `api_server.c` still
includes neither `modules/gpio.h` nor HAL.

### F.2 Framing additions

#### F.2.1 `Expect`

Any `Expect` header is answered `400 UNSUPPORTED_EXPECTATION` *before* the body is awaited.
`100-continue` is not implemented; refusing explicitly beats stalling a client that is
holding back its body until it sees a `100 Continue` that never comes.

#### F.2.2 `Content-Type`

Captured during header parsing (points into `req`, valid until dispatch). A duplicate
`Content-Type` header is `400`. The transport only *captures* the value; the router
decides whether the media type is acceptable for the route.

#### F.2.3 Body presence

`has_content_length` now distinguishes an absent `Content-Length` from an explicit
`Content-Length: 0`. Both yield zero body bytes, but the router reports them differently
for `PUT`.

| Framing | `body_present` | `body_len` | PUT result |
|---|---|---|---|
| `Content-Length` absent | 0 | 0 | `400 INVALID_JSON` — "Request body is required." |
| `Content-Length: 0` | 1 | 0 | `400 INVALID_JSON` — "Request body is empty." |

### F.3 Validation chain (both PUT routes)

Order is fixed and matches the mutation invariant; nothing before step 8 touches hardware.

1. Method + route shape — wrong method → `405` with a per-route `Allow`.
2. Pin-name syntax `P[A|B]<0-15>` — else `400 INVALID_PIN`.
3. `Content-Type: application/json` (parameters allowed) — else `415`.
4. Body presence — absent vs empty (§F.2.3).
5. JSON parse + field checks — else `400 INVALID_JSON`.
6. Field values — else `400 INVALID_VALUE`.
7. Pin allowlist via `GPIO_IsPinAllowed()` — else `404 PIN_NOT_FOUND`.
8. Value route only: `configured && mode == output` — else `409 HARDWARE_STATE_CONFLICT`.
9. `apply_*()` → `modules/gpio.c`.
10. On success, a **fresh** `GPIO_GetPinInfo()` builds the response; requested values are
    never echoed. A non-`OK` module result becomes `500 GPIO_OPERATION_FAILED` and can
    never be reported as success.

### F.4 Realized contract

```text
PUT  /api/v1/gpio/{pin}         -> 200  {"success":true,"data":{pin,configured,mode,pull,value}}
                                -> 400  INVALID_PIN | INVALID_JSON | INVALID_VALUE
                                -> 404  PIN_NOT_FOUND
                                -> 405  "Allow: GET, HEAD, PUT"
                                -> 415  UNSUPPORTED_MEDIA_TYPE
                                -> 500  GPIO_OPERATION_FAILED
PUT  /api/v1/gpio/{pin}/value   -> 200  same pin object
                                -> 400  INVALID_PIN | INVALID_JSON | INVALID_VALUE
                                -> 404  PIN_NOT_FOUND
                                -> 405  "Allow: PUT"
                                -> 409  HARDWARE_STATE_CONFLICT
                                -> 415  UNSUPPORTED_MEDIA_TYPE
                                -> 500  GPIO_OPERATION_FAILED
```

Request bodies (strict — unknown, duplicate, missing, and wrongly-typed fields all fail):

```json
{"mode":"input"}                          // or "output"
{"mode":"output","pull":"none"}           // pull: "none" | "up" | "down" (default "none")
{"value":0}                               // integer 0 or 1 only
```

### F.5 JSON reader strictness

The reader is deliberately narrow: one flat object, string or integer values only. No
general-purpose parser is present, so there is no extra surface to harden. Rejected as
`400 INVALID_JSON`: malformed syntax, non-object top level, trailing data, duplicate keys,
unknown fields, missing required fields, and wrong value *types* (`"mode":5`,
`"value":true`, `"value":"1"`, `"value":1.0`, leading zeros like `{"value":01}`). An
over-long string or a huge integer is a correctly-typed but unsupported value, so it maps
to `400 INVALID_VALUE` (`{"pull":"sideways"}`, `{"value":999999999999}`), not to a syntax
error. Strings are bounded by their destination buffers; an over-long string is consumed
and flagged (`JSON_ReaderTruncated`) rather than dropped, so the document still parses.

### F.6 Test transcript (host `192.168.7.2`, device `192.168.7.1`)

Validation-only half (before wiring), representative rows:

```text
PUT /api/v1/gpio/PA5  {"mode":"input"}                 -> 500 GPIO_OPERATION_FAILED  (reached the inert seam)
PUT /api/v1/gpio/PA5  {}                               -> 400 INVALID_JSON  missing mode
PUT /api/v1/gpio/PA5  {"mode":5}                       -> 400 INVALID_JSON  wrong type
PUT /api/v1/gpio/PA5  {"mode":"blink"}                 -> 400 INVALID_VALUE
PUT /api/v1/gpio/PA5  {"mode":"input","pull":"sideways"}-> 400 INVALID_VALUE
PUT /api/v1/gpio/PA5  {"mode":"input"}garbage          -> 400 INVALID_JSON  trailing data
PUT /api/v1/gpio/PB0/value {"value":2}                 -> 400 INVALID_VALUE
PUT /api/v1/gpio/PB0/value {"value":true}              -> 400 INVALID_JSON
PUT /api/v1/gpio/PA5/value {"value":1}                 -> 409 (PA5 not a configured output)
PUT /api/v1/gpio/PA5  (no Content-Type)                -> 415 UNSUPPORTED_MEDIA_TYPE
PUT /api/v1/gpio/PA5  (Expect: 100-continue)           -> 400 UNSUPPORTED_EXPECTATION (no body waited for)
PUT /api/v1/gpio/PA5  (no Content-Length)              -> 400 INVALID_JSON  "Request body is required."
PUT /api/v1/gpio/PA5  (Content-Length: 0)              -> 400 INVALID_JSON  "Request body is empty."
```

Hardware half (after wiring), full matrix on the board:

```text
GET  /api/v1/gpio/PA5                                  -> configured:false
PUT  /api/v1/gpio/PA5  {"mode":"input"}                -> 200 input  pull:none  value:0
PUT  /api/v1/gpio/PA5  {"mode":"output"}               -> 200 output pull:none  value:0   (reconfigure OK)
PUT  /api/v1/gpio/PA5  {"mode":"input","pull":"up"}    -> 200 input  pull:up    value:1   (internal pull-up)
PUT  /api/v1/gpio/PA5  {"mode":"input","pull":"down"}  -> 200 input  pull:down  value:0   (internal pull-down)
PUT  /api/v1/gpio/PA5  {"mode":"output","pull":"up"}   -> 200 output pull:up    value:0
PUT  /api/v1/gpio/PA5/value {"value":1}                -> 200 ... value:1          (read back via GET: value:1)
PUT  /api/v1/gpio/PA5/value {"value":0}                -> 200 ... value:0
PUT  /api/v1/gpio/PB0/value {"value":1}                -> 200 ... value:1          (PB0, output from GPIO_Init)
PUT  /api/v1/gpio/PB0/value {"value":0}                -> 200 ... value:0
PUT  /api/v1/gpio/PA6  {"mode":"input"}                -> 200 input
PUT  /api/v1/gpio/PA6/value {"value":1}                -> 409 HARDWARE_STATE_CONFLICT   (input pin)
GET  /api/v1/gpio/PA6                                  -> still configured input  (no metadata change)
PUT  /api/v1/gpio/PA7/value {"value":1}                -> 409 HARDWARE_STATE_CONFLICT   (unconfigured)
GET  /api/v1/gpio/PA7                                  -> still configured:false
PUT  /api/v1/gpio/PA0  {"mode":"output"}               -> 404 PIN_NOT_FOUND
GET  /api/v1/gpio/PA0                                  -> still 404 (not configured)
```

`value` is the sampled pad level (`IDR`), which is why the pull-up/pull-down reads differ
from the requested configuration. Reads after every success confirm the module's recorded
state, not the request echo.

Regression (unchanged read-only surface and legacy site):

```text
GET /api/v1/system          -> 200
GET /api/v1/gpio            -> 200, 946 B   (grew from 871 B: PA5/PA6 now configured)
GET /api/v1/gpio/PB0        -> 200 configured output
HEAD /api/v1/gpio           -> 200 + Content-Length, no body
legacy HTTPD :80 /          -> 200, 3142 B
```

### F.7 Footprint

```text
RAM:   52200 B / 128 KB (39.83%)   # was 52184 B (39.81%)
FLASH: 127024 B / 512 KB (24.23%)  # was 121372 B (23.15%)
```

The body parser, the JSON reader, and the two mutation handlers add ~5.6 KB flash; RAM is
essentially flat (the request-body buffers already existed). All buffers remain static;
no `malloc` was introduced. Peak stack was not re-measured and should be revisited as the
parser grows (see Appendix A).

### F.8 Decisions recorded

- Success is `200` for both routes, returning the resulting pin object. Even a first-time
  configuration is an update to a known GPIO resource URI, not creation of one, so `201`
  is not used. Consistent responses also simplify the client.
- Unknown JSON fields are rejected (`400 INVALID_JSON`) rather than ignored, so a
  misspelling such as `"pul":"up"` fails instead of silently applying a default.
- `409 HARDWARE_STATE_CONFLICT` distinguishes a state conflict (writing a non-output pin)
  from malformed input; it is outside Chapter 5's earlier "at least" list and is used here
  deliberately.
- Type vs value: wrong JSON type → `INVALID_JSON`; correctly typed but unsupported value
  → `INVALID_VALUE`. `-1` is an unsupported integer value; `1.0` is an invalid integer
  representation. Both are consistent with a deliberately narrow contract.
- `GPIO_ConfigurePin()` may safely reconfigure a configured pin (it re-runs
  `HAL_GPIO_Init()`) and updates its tracking arrays only after that call, so a failed
  operation cannot leave partial metadata. The value route keeps the output-mode check
  immediately before `apply_value()`; the cooperative single-threaded loop leaves no
  window between validation and the GPIO call.

### F.9 Still to come

Deconfiguration (no rollback yet: a pin configured through the API can be reconfigured but
not returned to unconfigured), `204` for a future deconfigure route, event notifications,
and WebSocket support remain out of scope for this increment.

## Appendix G — Verified increment log: transport throughput benchmark

Before writing any high-speed acquisition (logic analyzer, oscilloscope, SPI) code, we
measured what the existing USB NCM + lwIP transport actually sustains. The measurement is
done by a **temporary, bounded benchmark endpoint** on the existing API server, so the test
exercises the real production path (same PCBs, same send buffers, same superloop) rather
than a synthetic loop. No acquisition logic lives in the transport.

### G.1 What changed

| File | Change |
| --- | --- |
| `Core/Inc/api_benchmark.h`, `Core/Src/api_benchmark.c` | New. Pattern generation, allowed-size allowlist, single-session gate, last-session statistics. No lwIP, no I/O. |
| `Core/Inc/api_router.h` | `API_Route_t` gained `stream_bytes` and `content_type`. |
| `Core/Src/api_router.c` | Two routes, query parsing and validation; every other route zeroes the streaming descriptor. |
| `Core/Src/api_server.c` | `API_CONN_STREAM` state, the streaming pump, session gate acquire/release, and teardown accounting. |
| `CMakeLists.txt` | New sources registered. |
| `tools/bench_throughput.py` | New host-side verifier and timer (stdlib only). |

The transport was extended, not duplicated: the benchmark rides the same `api_conn_t`
slots and the same `lwip/tcp.h` callbacks as the JSON API.

### G.2 Streaming contract

```text
GET  /api/v1/benchmark/stream?bytes=N   -> 200 application/octet-stream, body N bytes
HEAD /api/v1/benchmark/stream?bytes=N   -> 200, Content-Length: N, no body, no gate
GET  /api/v1/benchmark/stats            -> last recorded session as JSON
```

- Allowed sizes: `4096`, `16384`, `32768`, `65536`, `131072`. Anything else →
  `400 INVALID_VALUE`. A missing or malformed query → `400 INVALID_QUERY`; a non-GET/HEAD
  method → `405`.
- The body is a **32-bit little-endian counter**: word `w` (body bytes `4w..4w+3`) equals
  `w`, low byte first. A single comparison on the host detects missing, duplicated,
  reordered and corrupted bytes.
- `elapsed_ms` / `retries` / `completed` describe the last session, whether it finished or
  was interrupted, and they survive the connection closing. Before any session,
  `/stats` returns `404 NO_SESSION`.

Statistics body (exact shape):

```json
{"success":true,"data":{"requested_bytes":65536,"acked_bytes":65536,
 "elapsed_ms":125,"retries":3,"completed":true}}
```

`requested_bytes` and `acked_bytes` count **body bytes only**; the HTTP header is not part
of the payload measurement, so a completed session always shows them equal.

### G.3 Session lifecycle and safe cleanup

One benchmark at a time. The gate is acquired only for a real (non-HEAD) stream; a second
concurrent stream gets `409 BENCHMARK_BUSY`. All teardown funnels through `conn_reset()`,
which calls `bench_finalize()` first, so the gate is released **exactly once** for every
exit path:

| Exit path | Handled by | Recorded |
| --- | --- | --- |
| Normal completion | `api_sent()` → `conn_close()` | `completed: true` |
| Client FIN (half close) | `api_recv()` keeps draining | completion still detected |
| Client RST | `tcp_err()` (`api_err`) → `conn_reset` | `completed: false` |
| Send timeout | `api_poll()` → `conn_abort()` | `completed: false` |
| Lifetime backstop | `api_poll()` age cap | `completed: false` |
| Slot reuse | `conn_reset()` | gate already free |

`HEAD` composes the identical header but sends it through the ordinary buffered path, so it
neither acquires the gate nor generates a body, and it does not disturb the recorded
statistics.

### G.4 Three distinct apply counters

The pump keeps the three stages the review asked for, in bytes:

- `stream_generated` — staged into the `body` staging buffer.
- `stream_written` — accepted by `tcp_write()` (advances only on success).
- body-acked — cumulative ACKed, derived from the `tcp_sent()` callback.

`sendbuf` room does **not** guarantee `tcp_write()` success: lwIP can still return `ERR_MEM`
on segment/pool exhaustion, so `ERR_MEM` from either `tcp_write()` or `tcp_output()` is
treated as "retry from `api_sent()`/`api_poll()`" and the chunk is regenerated from
`stream_written`. Because the pattern is always regenerated from the current offset, a
chunk that failed to queue is never counted twice nor written at the wrong offset.

### G.5 Test transcript (host `192.168.7.2`, device `192.168.7.1`)

```text
payload verification + throughput (body-only and end-to-end)
    size     rep     body_ms   body_MB/s      e2e_ms    e2e_MB/s  ok
    4096       1       3.868       1.059       9.023       0.454  yes
    4096       5       3.885       1.054       8.764       0.467  yes
   16384       5      22.501       0.728      27.681       0.592  yes
   32768       5      47.182       0.695      52.294       0.627  yes
   65536       5      96.859       0.677     102.018       0.642  yes
  131072       5     197.575       0.663     202.734       0.647  yes

    size      min_MB/s      med_MB/s      max_MB/s  e2e_med_MB/s
    4096         1.010         1.054         1.109         0.467
   16384         0.720         0.727         0.728         0.590
   32768         0.690         0.694         0.696         0.625
   65536         0.675         0.678         0.680         0.643
  131072         0.663         0.665         0.668         0.648

concurrent normal requests during a transfer:
  stream opened: status=200 content-length=131072
  during transfer GET /api/v1/system   -> 200
  during transfer GET /api/v1/gpio     -> 200
  stream payload after probes: ok

concurrent benchmark gate:
  second stream while busy -> 409 {"code":"BENCHMARK_BUSY"}
  stream after first closed  -> 200 (4096 bytes ok)

abort mid-transfer then restart:
  aborted after 1460 bytes
  restart -> 200 (4096 bytes ok)
```

Additional checks: `stats before any session` → `404 NO_SESSION`; `HEAD` while a stream is
active → `200 Content-Length: 131072` (not gated); `HEAD` does not overwrite statistics;
legacy site on `:80` → `200` (3142 B); GPIO configure/write/read → `200` with fresh
read-back. Every one of the 25 payload transfers passed full-pattern verification.

### G.6 Throughput results

Sustained body rate, median of 5 runs:

| Size | body MB/s | end-to-end MB/s | body KB/s |
| --- | --- | --- | --- |
| 4 KB | 1.05 | 0.47 | 1079 |
| 16 KB | 0.73 | 0.59 | 744 |
| 32 KB | 0.69 | 0.63 | 711 |
| 64 KB | 0.68 | 0.64 | 694 |
| 128 KB | 0.665 | 0.65 | 681 |

`body` times only the payload (first body byte → last body byte); `end-to-end` includes the
request and header round trip.

### G.7 Analysis

- **Sustained ceiling ≈ 0.66 MB/s (≈ 680 KB/s).** The STM32F411 OTG_FS is USB Full Speed
  (12 Mbit/s ⇒ 1.5 MB/s raw). The firmware sustains about **44 % of the raw FS bandwidth**
  after NCM framing, lwIP segmentation and the cooperative 1 ms tick.
- **The 4 KB number is not representative.** For a small transfer the measured body window
  starts after the first ACKed segment and the rest drains from the send buffer, so it
  looks faster than the link. The asymptotic rate is the 128 KB value. End-to-end at 4 KB is
  dominated by a fixed ~5 ms request/header round trip.
- **Retries were 0 across the sweep.** `retries` counts only attempts that got a real
  `ERR_MEM` back from `tcp_write()`/`tcp_output()` (memory/segment-pool exhaustion). A host
  reading continuously keeps the window open, so that path is not exercised; a client that
  reads slowly will raise it. An earlier draft also counted send-buffer-full pump calls and
  reported one "retry" per TCP segment (~86 for 128 KB); that conflated ordinary
  backpressure with `ERR_MEM` and has been corrected to match the agreed contract.
- **Baseline established.** These numbers, not the theoretical 1.5 MB/s, are the budget for
  continuous acquisition. At ~0.66 MB/s the transport can move roughly 660 KB/s of sample
  data; a design that needs more must either compress/decimate at the source or the
  transport buffers must be revisited (explicitly deferred — measure first, tune later).

### G.8 Footprint

```text
RAM:   52264 B / 128 KB (39.87%)   # was 52200 B (39.83%)  -> +64 B
FLASH: 131100 B / 512 KB (25.01%)  # was 127024 B (24.23%) -> +4.0 KB
```

RAM adds only the per-slot streaming counters and the 16-byte statistics record; the
staging buffer is the existing `body[2048]` reused verbatim, so no second 1460/2048-byte
buffer was allocated. Most of the flash delta is the pump, the stream state machine and the
router's string handling. Everything remains static — no `malloc`.

### G.9 Device logging caveat

`bench_finalize()` emits exactly one `BENCH ...` line per session through `printf`. In this
build `printf` had no back-end: `Core/Src/syscalls.c` routes `_write` through a weak,
undefined `__io_putchar`, and the compiler lowered that call to `nop.w`, so the line went
nowhere. `/api/v1/benchmark/stats` was the working firmware-side diagnostic. Appendix H
adds an opt-in SWO/ITM back-end for `printf` as a separate infrastructure change.

### G.10 Reproduce

```bash
# build + flash
cmake --preset Debug && cmake --build --preset Debug
~/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
    -c port=SWD freq=4000 -w build/Debug/usb-webserver.elf -v -rst

# measure (5 repeats per size, plus lifecycle tests)
python3 tools/bench_throughput.py --repeats 5 --extra
```

### G.11 Still to come

The endpoint is temporary and bounded; it stays until the first acquisition path has its own
trustworthy consumer. Explicitly deferred: tuning `TCP_MSS` / `TCP_SND_BUF` / NCM NTB sizes.
Capture, recording and streaming of real acquisition data come next, budgeted against the
~0.65 MB/s sustained figure measured here.

## Appendix H — Verified increment log: opt-in SWO/ITM console

Separate, small infrastructure change: give `printf` a real byte sink so the one-line
`BENCH` log (and any future diagnostics) can actually be read. It is deliberately opt-in so
the default and release builds keep a harmless stub and no build can stall on a debugger.

### H.1 What changed

| File | Change |
| --- | --- |
| `Core/Src/log_itm.c` | New. Strong `__io_putchar()`: ITM stimulus port 0 when enabled, no-op otherwise. |
| `CMakeLists.txt` | `USB_WS_ENABLE_ITM_LOG` option (default `OFF`), source registration, and a conditional `USB_WS_ENABLE_ITM_LOG=1` define. |
| `CMakePresets.json` | New `DebugITM` configure/build preset that turns the option on. |

`syscalls.c` is untouched: it already declares `__io_putchar` weak, so a strong definition
elsewhere simply replaces the call the compiler had reduced to `nop.w`.

### H.2 Design

- **Opt-in.** `USB_WS_ENABLE_ITM_LOG=OFF` (default) compiles a stub that returns without
  writing; `printf` then costs a function call and nothing else.
- **Guard before writing.** The ITM path checks both `ITM_TCR_ITMENA_Msk` and `ITM_TER`
  bit 0 and only writes when tracing is actually enabled by a debugger. With no debugger,
  nothing is written.
- **Bounded wait.** The FIFO spin is capped, so a debugger that stops draining SWO can cost
  a dropped byte but can never stall the cooperative superloop.
- **No hot-path cost.** `printf` is only used on init/error paths and one line per benchmark
  session in teardown, so enabling it cannot perturb a transfer.

### H.3 Build verification

Both presets build warning-free. The default-sink difference is visible in the disassembly:

```text
Debug     __io_putchar  0x16 bytes   no CoreSight access  (harmless stub)
DebugITM  __io_putchar  0x62 bytes   touches 0xE0000000   (real ITM write)
```

`_write` calls `__io_putchar` in both builds (previously the call was a `nop.w`).

### H.4 SWO capture attempt — blocked by the probe

Capturing the trace on this bench was **not** possible. Using the bundled
`ST-LINK_gdbserver` with SWV (`-d -a 96000000 -b 96 -z 61235`) the SWO socket came up and
reported 1 MHz, but no bytes were ever produced. Reading the CoreSight registers over GDB
showed why tracing stayed silent even after correcting the TPIU:

```text
CoreDebug_DEMCR = 0x01000000   # TRCENA on
ITM_TCR         = 0x00000000   # ITMENA off  -> debugger never enabled ITM
ITM_TER         = 0x00000000   # no stimulus port enabled
TPIU_ACPR       = 0x00000000
```

After enabling ITM and configuring TPIU by hand (`TPIU_SPPR=1` NRZ, `TPIU_ACPR=0x5F` ⇒
1 MHz, `ITM_TER=1`, `ITM_TCR=1`) the ITM was genuinely running — `ITM_TCR` read back
`ITMENA=1` — yet still zero trace bytes reached the host. The conclusion is the one the
review flagged: **this ST-Link V2 clone does not deliver SWO reliably**, so connecting over
SWD does not imply a working trace pin. The firmware side is correct; the probe side is the
limitation. On a probe with a working SWO path (genuine ST-Link/V2-1, V3, or J-Link) the
same build streams ITM port 0 at 1 MHz.

### H.5 Independent verification (no SWO needed)

Because the trace link is unusable here, the log line was verified at the software level:
GDB trapped `_write()` and dumped the exact buffer handed to it for one 64 KB session.

```text
WRITE[102]: BENCH requested=65536 generated=65536 written=65536 acked=65536 elapsed_ms=105 retries=0 completed=1
```

This confirms end to end that the firmware produces one correctly formatted line per
session (not per byte) with consistent counters. `/api/v1/benchmark/stats` remains the
switch-independent, authoritative device-side surface.

### H.6 Regression — transport unchanged when logging is off

With the default build (logging `OFF`) reflashed, the sweep is statistically unchanged and
`retries` is 0 at every size:

| Size | body MB/s (baseline → now) | e2e MB/s (now) | retries |
| --- | --- | --- | --- |
| 4 KB | 1.05 → 1.02 | 0.46 | 0 |
| 16 KB | 0.73 → 0.71 | 0.58 | 0 |
| 32 KB | 0.69 → 0.68 | 0.61 | 0 |
| 64 KB | 0.68 → 0.66 | 0.63 | 0 |
| 128 KB | 0.665 → 0.648 | 0.63 | 0 |

The small spread is run-to-run variance, not a code effect; the concurrency, gate and abort
tests all still pass. The board is left flashed with the default (logging `OFF`) build.

### H.7 Footprint

```text
logging OFF:  RAM 52264 B / 128 KB (39.87%)   FLASH 131016 B / 512 KB (24.99%)
logging ON:   RAM 52264 B / 128 KB (39.87%)   FLASH 131092 B / 512 KB (25.00%)
```

The SWO/ITM back-end adds 76 bytes of flash over the no-op stub. RAM is unchanged; both are
stateless and use no buffer.

---

## Chapter 11 — Burst acquisition (ADC + timer-triggered DMA)

The transport chapters measure how fast bytes can move. This chapter is about
acquiring *measurements*: sampling an analog input at a fast, fixed rate for a
short window and then shipping the captured buffer to the browser.

The model is an **oscilloscope**, not a live stream. Sample fast for a defined
window into SRAM, then transfer. USB does not have to keep up in real time, so
the acquisition rate is set by the ADC and a timer, never by the network.

### 11.1 Hardware path

```
PA0 ──▶ ADC1 (one channel)
          ▲
TIM2 TRGO ┘            every TRGO: one conversion
          │
          ▼
        DMA2 Stream 0 ──▶ acq_buffer[] in SRAM
```

- **TIM2** runs from the 96 MHz APB1 timer clock and emits an update-event
  TRGO at the requested rate. It is the sample clock.
- **ADC1** is set for 12-bit, single channel (IN0), external trigger = TIM2
  TRGO rising edge, DMA enabled. With ADCCLK = PCLK2/4 = 24 MHz.
- **DMA2 Stream 0 / Channel 0** moves each conversion from `ADC1->DR` into a
  fixed `uint16_t` buffer in normal (one-shot) mode. The CPU never touches an
  individual sample.

The engine lives in `Core/Src/modules/acquisition.c`. It includes no lwIP or USB
header: it is a pure producer, and the transport is a separate consumer.

### 11.2 Sample rate and window

The **rate** is the timer period. The **sample window** (ADC sample time) is
chosen as the *longest* one whose conversion still fits inside one trigger
period — a longer window means a lower source-impedance requirement.

Conversion time for a 12-bit sample is `(sample_cycles + 12) / 24 MHz`. A window
fits when `(sample_cycles + 12) * 4 <= timer_ticks`. So:

| rate | timer ARR | sample window | conversion |
|------|-----------|---------------|------------|
| 1.600 MS/s | 59 | 3 cycles | 0.625 μs |
| 1.000 MS/s | 95 | 3 cycles | 0.625 μs |
| 0.500 MS/s | 191 | 28 cycles | 1.667 μs |
| 0.250 MS/s | 383 | 84 cycles | 4.000 μs |
| 0.100 MS/s | 959 | 144 cycles | 6.500 μs |

A rate is accepted only if it divides the 96 MHz timer clock exactly, so the
period is precise; anything else is `400`.

### 11.3 API surface

All under `/api/v1/acquisition`:

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/capture` | Start a burst. Body `{"rate_hz":N,"samples":M}`. → `202 Accepted` |
| `GET`  | `/status`  | State, counters and timing. → `200` |
| `GET`  | `/data`    | Stream the raw samples as `application/octet-stream`. → `200` |
| `DELETE` | `/`      | Discard the capture, return to `idle`. → `204` |

Start a capture and poll:

```console
$ curl -s -X POST 192.168.7.1:8080/api/v1/acquisition/capture \
       -H 'Content-Type: application/json' \
       -d '{"rate_hz":1000000,"samples":24576}'
{"success":true,"data":{"state":"capturing","capture_id":1,
 "rate_hz":1000000,"samples":24576}}

$ curl -s 192.168.7.1:8080/api/v1/acquisition/status
{"success":true,"data":{"state":"complete","capture_id":1,
 "rate_hz":1000000,"capacity":24576,"captured":24576,
 "sample_bytes":2,"resolution_bits":12,"reference_mv":3300,
 "overrun":false,"dma_errors":0,"timeouts":0,"tail_overruns":0,
 "elapsed_us":24577}}
```

`POST` returns immediately; the capture runs in the background and never blocks
the main loop or a TCP request. `elapsed_us` is the measured window, taken with
the DWT cycle counter.

### 11.4 States and ownership

```
idle ──POST──▶ capturing ──done──▶ complete ──GET /data──▶ streaming ──┐
  ▲                                  │                                  │
  └──────────── DELETE ──────────────┴──────────────────────────────────┘
                                     └── fault ──▶ error ──DELETE──▶ idle
```

`complete` **holds the client's data**. A new `POST` while `complete` or
`streaming` is refused with `409 ACQUISITION_BUSY` rather than silently
overwriting the buffer. `DELETE` is required before the next capture. The
transport pins the buffer (`COMPLETE → STREAMING`) only after it has taken the
shared single-session bulk gate, and releases it exactly once on close, RST or
timeout, so a slow client can never block the producer.

### 11.5 Raw samples, not millivolts

`/data` streams `captured` little-endian `uint16` values, each a right-aligned
12-bit code `0..4095`. The acquisition path stores exactly what the ADC
produced; conversion to a voltage is the client's business using `reference_mv`
and `resolution_bits`. This keeps the fast path free of arithmetic and preserves
the original measurement.

### 11.6 Configuring the input

PA0 / ADC1_IN0 is a plain analog input:

- It is **reserved**: it is outside the GPIO whitelist in `modules/gpio.c`, so the
  GPIO API can never reconfigure or drive it. The acquisition module owns it.
- With nothing connected, a capture reads input noise (roughly 650..1340 codes
  in the bring-up). The timing and transport behaviour are independent of the
  signal.

### 11.7 Recognised errors

| Condition | Response |
|-----------|----------|
| Rate not a divisor of 96 MHz, or out of range | `400 INVALID_VALUE` |
| `samples` 0 or > `capacity` | `400 INVALID_VALUE` |
| Malformed / unknown JSON field | `400 INVALID_JSON` |
| Wrong content type | `415 UNSUPPORTED_MEDIA_TYPE` |
| Wrong method for the route | `405 METHOD_NOT_ALLOWED` |
| Capturing / streaming already active | `409 ACQUISITION_BUSY` |
| No completed capture to read | `404 NO_DATA` |

---

## Appendix I — Verified increment log: burst acquisition

Branch `acquisition-dma-capture`, build `Debug`. Device `192.168.7.1`, host
`192.168.7.2/24`.

### I.1 What changed

| File | Change |
|------|--------|
| `Core/Inc/modules/acquisition.h` | Public engine interface and limits |
| `Core/Src/modules/acquisition.c` | Bare-metal ADC1/TIM2/DMA2 engine + IRQ handlers |
| `Core/Inc/hardware_config.h` | `ENABLE_ADC` 0 → 1 |
| `Core/Src/hardware.c` | `ACQ_Init()` / `ACQ_Task()` hooks |
| `Core/Src/modules/gpio.c` | Documents PA0 as reserved (comment only) |
| `Core/Inc/api_router.h` | `API_Route_t.stream_src` (pattern vs buffer) |
| `Core/Src/api_router.c` | Four acquisition routes + JSON |
| `Core/Src/api_server.c` | Buffer-backed stream; acquisition session ownership |
| `CMakeLists.txt` | Adds the acquisition module |
| `tools/acq_capture.py` | Host capture client |
| `docs/ACQUISITION_DESIGN.md` | Design + bring-up results |

No `.ioc`, `stm32f4xx_hal_conf.h` or `cmake/stm32cubemx/CMakeLists.txt` change:
the vendored HAL has no ADC/TIM module, so the engine is register-level against
the CMSIS device header. lwIP buffer sizes and the benchmark path are untouched.

### I.2 Two faults found on hardware

**F1 — only the first burst after reset completed.** The next capture hung until
the watchdog aborted it. During the hang the timer ran, the DMA stream was
enabled and the ADC DMA mode was set, yet `NDTR` never moved. `ADC_CR2.DDS=0`
means the ADC issues no new DMA request after the last transfer of a burst, so
the converter was left "DMA-done". Fixed by setting `DDS=1`.

**F2 — a tail overrun could mark a completed burst as `ERROR`.** A trigger
arriving as the burst ended completed a conversion with no DMA slot left,
setting `OVR`; if the ADC interrupt ran first it won the state. Fixed by giving
the DMA completion interrupt priority over the overrun interrupt and by treating
an overrun as fatal only while `NDTR != 0`. Post-burst overruns are counted as
`tail_overruns`.

### I.3 Timing transcript

```
burst acquisition: rate=1000000 Hz samples=24576 (49152 bytes)
 rep     capt_ms      exp_ms     xfer_ms   xfer_MB/s         min     max    ovr
   1      24.577      24.576      74.686       0.658         858        1339      no      0
   2      24.577      24.576      74.287       0.662         653        1337      no      0
   ...
  40      24.577      24.576      74.462       0.660         686        1338      no      0
  samples=24576 mean=1298.0 span=24.576 ms state=complete
```

Rate sweep (24 576 samples), measured vs expected:

| rate | expected | measured |
|------|----------|----------|
| 1.600 MS/s | 15 360 μs | 15 362 |
| 1.000 MS/s | 24 576 μs | 24 577 |
| 0.500 MS/s | 49 152 μs | 49 152 |
| 0.250 MS/s | 98 304 μs | 98 302 |
| 0.100 MS/s | 245 760 μs | 245 749 |
| 0.050 MS/s | 491 520 μs | 491 509 |

### I.4 Concurrency and regression

```
normal traffic during a data transfer:
  data stream opened: status=200 content-length=49152
  during transfer GET /api/v1/system               -> 200
  during transfer GET /api/v1/gpio                 -> 200
  during transfer GET /api/v1/acquisition/status   -> 200
  stream payload after probes: ok (49152/49152 bytes)
```

Benchmark unchanged: 128 KB body 0.647 MB/s (baseline 0.648).

### I.5 Footprint

```text
RAM   101464 B / 128 KB (77.4%)    FLASH 136904 B / 512 KB (26.1%)
acq_buffer = 49152 B (48 KB), ACQ_MAX_SAMPLES = 24576
```

### I.6 Reproduce

```bash
cmake --preset Debug && cmake --build --preset Debug
STM32_Programmer_CLI -c port=SWD freq=4000 \
  -w build/Debug/usb-webserver.elf -v -rst

python3 tools/acq_capture.py --repeats 5            # timing + throughput
python3 tools/acq_capture.py --concurrent           # normal traffic during a stream
```
