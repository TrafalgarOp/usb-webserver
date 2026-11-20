# Burst Acquisition Design (ADC + Timer-Triggered DMA)

Status: **design proposal — for review before implementation.**
Baseline: tag `bench-baseline` (USB transport ≈ 0.65 MB/s measured, 0.5 MB/s planning budget).

## 1. Central assumption

Acquisition is **burst / oscilloscope-style**, not real-time streaming.

- The ADC fills a RAM buffer at a high sample rate while the USB link is idle.
- When the capture window is complete, the firmware *then* transfers the stored samples to the browser.
- The USB link therefore never has to keep up with the ADC in real time. **The sampling rate
  is limited only by the ADC and by RAM; the transfer time is limited only by USB.**

This decoupling is the whole point: it lets us sample far faster than 0.65 MB/s, as long as the
burst fits in RAM.

---

## 2. Hardware limits — maximum practical ADC rate

### 2.1 Clock path (confirmed from `usb-webserver.ioc`)

| Node | Value | Source |
|------|-------|--------|
| SYSCLK | 96 MHz | `RCC.SYSCLKFreq_VALUE` |
| HCLK | 96 MHz | `RCC.HCLKFreq_Value` |
| PCLK2 (APB2) | 96 MHz | `RCC.APB2Freq_Value` |
| ADCCLK prescaler | /2, /4, /6, /8 | STM32F411 ADC prescaler |

STM32F411 maximum ADCCLK is **36 MHz**. With PCLK2 = 96 MHz the legal options are:

| Prescaler | ADCCLK | Legal? |
|-----------|--------|--------|
| /2 | 48 MHz | ✗ exceeds 36 MHz |
| **/4** | **24 MHz** | ✓ **maximum legal** |
| /6 | 16 MHz | ✓ |
| /8 | 12 MHz | ✓ |

**Maximum achievable ADCCLK on this clock tree = 24 MHz.** (The 36 MHz / 2.4 MS/s datasheet
figure is unreachable because 96 MHz does not divide to 36 MHz with a legal prescaler.)

### 2.2 Conversion time and sample rate

12-bit conversion: `Tconv = (N_sample + 12) / ADCCLK`. Rate `f_s = ADCCLK / (N_sample + 12)`.

At **ADCCLK = 24 MHz**:

| N_sample (cycles) | Tconv (cycles) | Tconv (ns) | f_s (MS/s) |
|-------------------|----------------|------------|------------|
| 3 | 15 | 625 | **1.600** ← ceiling |
| 15 | 27 | 1125 | 0.889 |
| 28 | 40 | 1667 | 0.600 |
| 56 | 68 | 2833 | 0.353 |
| 84 | 96 | 4000 | 0.250 |
| 112 | 124 | 5167 | 0.194 |
| 144 | 156 | 6500 | 0.154 |
| 480 | 492 | 20500 | 0.0488 |

At ADCCLK = 16 MHz (/6), for reference:

| N_sample | f_s (MS/s) |
|----------|------------|
| 3 | 1.067 |
| 15 | 0.593 |
| 28 | 0.400 |
| 56 | 0.235 |
| 84 | 0.167 |

**Answer to "maximum practical rate": ≈ 1.6 MS/s** (12-bit, 3-cycle sample window, ADCCLK 24 MHz).
For a cleaner/quieter sample window, **1.0 MS/s** with a 12-cycle window (24 total cycles) is the
recommended ceiling; higher sample windows trade rate for source impedance tolerance.

### 2.3 Timer rate generation

TIM2 (32-bit, APB1). APB1 prescaler = /2, so the **APB1 timer clock = 96 MHz**. Update rate:

```
f_s = 96 MHz / ((PSC+1) * (ARR+1))
```

All target rates divide evenly:

| f_s | PSC | ARR |
|-----|-----|-----|
| 1.600 MHz | 0 | 59 |
| 1.000 MHz | 0 | 95 |
| 0.500 MHz | 0 | 191 |
| 0.250 MHz | 0 | 383 |
| 0.100 MHz | 0 | 959 |

TIM2 TRGO = update event drives ADC1 regular conversions via the external-trigger input
(HAL `ADC_EXTERNALTRIGCONV_T2_TRGO`), so the CPU never touches an individual sample.

---

## 3. Memory budget

### 3.1 Current firmware RAM (tag `bench-baseline`)

| Region | Bytes |
|--------|-------|
| `.data` | 316 |
| `.bss` | 50 412 |
| `._user_heap_stack` (reserved) | 1 536 |
| **Static total** | **52 264** |
| Total SRAM (STM32F411CE) | 131 072 |

Largest existing consumers (relevant because they bound the free space):

| Symbol | Bytes | Purpose |
|--------|-------|---------|
| `ncm_epbuf` | 12 304 | TinyUSB NCM endpoint buffers |
| `memp_memory_PBUF_POOL_base` | 12 259 | lwIP pbuf pool |
| `api_conns` | 10 880 | REST connection slots |
| `ram_heap` | 8 779 | lwIP heap |
| `httpd_req_buf` | 1 024 | legacy lwIP httpd |
| `memp_memory_TCP_PCB_base` | 783 | lwIP TCP PCBs |

Free RAM below `_estack` = `131072 − 52264` = **78 808 B**. The runtime stack grows down from
`_estack` and draws from this same free region.

### 3.2 Sample buffer sizing (16-bit samples, 2 B each)

| Buffer | Bytes | Samples | Static total | % RAM | Free left | USB transfer @0.65 MB/s |
|--------|-------|---------|--------------|-------|-----------|--------------------------|
| 32 KB | 32 768 | 16 384 | 85 032 | 64.9 % | 36 040 | 50.4 ms |
| 48 KB | 49 152 | 24 576 | 101 416 | 77.4 % | 19 656 | 75.6 ms |
| **64 KB** | **65 536** | **32 768** | **117 800** | **89.9 %** | **13 272** | **100.8 ms** |
| 68 KB | 69 632 | 34 816 | 121 896 | 93.0 % | 9 176 | 107.1 ms |
| 96 KB | 98 304 | 49 152 | 150 328 | — | ✗ | does not fit |

**Proposed default: 64 KB / 32 768 samples (89.9 % RAM).** This leaves 13 272 B for the runtime
stack and margin — comfortable for a non-RTOS superloop whose deepest path is the USB/lwIP
callback chain (currently ≲3 KB). 68 KB is feasible but leaves little stack headroom; 96 KB is
not possible.

### 3.3 Answer to "maximum capture duration"

At 32 768 samples:

| f_s | Duration |
|-----|----------|
| 1.600 MS/s | 20.48 ms |
| 1.000 MS/s | 32.77 ms |
| 0.889 MS/s | 36.86 ms |
| 0.500 MS/s | 65.54 ms |
| 0.250 MS/s | 131.07 ms |
| 0.100 MS/s | 327.68 ms |

(With the 32 KB buffer, halve the durations; with 68 KB, scale by ≈1.06.)

---

## 4. Capture engine architecture

### 4.1 Producer / consumer split (strict)

```
        TIM2 TRGO ──► ADC1 ──DMA──► acq_buffer[0..N-1]
                                        │
                                        │  (read-only, only when COMPLETE)
                                        ▼
                          api_server stream pump ──► TCP ──► browser
```

- **DMA owns the buffer** while `ARMED`/`CAPTURING`.
- **The transport owns it read-only** while `COMPLETE`/`STREAMING`.
- These two windows are mutually exclusive by state. DMA never waits on the socket; the socket
  never reads a buffer the DMA is writing.

### 4.2 State machine

```
IDLE ──configure──► ARMED ──trigger──► CAPTURING ──DMA complete──► COMPLETE
  ▲                   │                     │                          │
  │                   │                     │ overrun/DMA error        │ GET /data
  │                   └──────────┬──────────┴──────────► ERROR         ▼
  │                              │                                  STREAMING
  └────────── DELETE ────────────┴──────────────────────────────────────┘
                                 (STREAMING → COMPLETE when the client closes)
```

- `IDLE`: buffer free; configuring is allowed.
- `ARMED`: DMA armed, waiting for the trigger. (For software trigger this is instantaneous.)
- `CAPTURING`: conversions flowing into the buffer.
- `COMPLETE`: capture done, data valid and stable. New capture allowed (overwrites → back to
  `ARMED`), but only when **not** `STREAMING`.
- `STREAMING`: transport is reading. New capture refused (`409 BUSY`).
- `ERROR`: overrun or DMA fault; buffer contents are suspect; status reports why.

### 4.3 DMA mode, trigger, and pre/post-trigger

- **Fixed capture (phase 1):** DMA **normal** mode, `NDTR = N`. One TIM2 TRGO per sample; on
  the Nth the DMA transfer-complete interrupt moves the engine to `COMPLETE`. Simple, exact.
- **Trigger (phase 1):** software. `POST /capture` arms and immediately starts the timer.
- **External trigger (phase 2, optional):** a GPIO edge (EXTI) sets a flag; firmware starts
  TIM2/DMA. Gives a real oscilloscope-style sync input.
- **Pre/post-trigger (phase 2, optional):** run TIM2 continuously, DMA **circular** over the
  whole buffer. On trigger, latch the current DMA position (`NDTR`) and count `post` further
  samples, then stop. The buffer then holds the last `pre` samples *before* the trigger plus
  `post` after. Bounded and CPU-free; adds complexity, so it is deferred.

### 4.4 Overrun / fault detection

| Condition | Detected via | Reported |
|-----------|--------------|----------|
| ADC overrun (conversion not read before next) | `ADC_FLAG_OVR` | `overrun:true` |
| DMA transfer error | `DMA_FLAG_TEIF*` | `dma_errors++` |
| DMA FIFO error | `DMA_FLAG_FEIF*` | `dma_errors++` |
| DMA direct-mode error | `DMA_FLAG_DMEIF*` | `dma_errors++` |
| Requested rate above 1.6 MS/s | config-time range check | `400 INVALID_VALUE` |
| Requested `samples` > capacity | config-time range check | `400 INVALID_VALUE` |
| Capture/stream already active | state check | `409 BUSY` |

The overrate case is also a deliberate test vector for the experiment: request 2 MS/s and
confirm the ADC reports an overrun rather than silently producing garbage.

---

## 5. Proposed REST surface

Base unchanged (`/api/v1`), envelope unchanged, status codes drawn from the existing set
(`200/201/204/400/404/405/409/413/415/500`) plus **`202`** for an accepted async capture.

| Method | Path | Body / result |
|--------|------|---------------|
| `POST` | `/api/v1/acquisition/capture` | `{"rate_hz":1000000,"samples":32768,"trigger":"software","edge":"rising","pretrigger":0}` → `202` `{"data":{"state":"armed","capture_id":1,"rate_hz":1000000,"samples":32768}}` |
| `GET` | `/api/v1/acquisition/status` | `200` `{"data":{"state":"complete","capture_id":1,"capacity":32768,"captured":32768,"rate_hz":1000000,"trigger_index":0,"overrun":false,"dma_errors":0,"elapsed_ms":32}}` |
| `GET` | `/api/v1/acquisition/data` | `200 application/octet-stream`, `Content-Length: 65536`, little-endian `uint16` samples (12-bit, right-aligned 0..4095) |
| `DELETE` | `/api/v1/acquisition` | `204`, reset to `IDLE` |

Error cases: `400 INVALID_JSON` / `INVALID_VALUE` (rate or count out of range, bad enum),
`409 BUSY` (capture or stream in progress), `404 NO_DATA` (no completed capture to read),
`405 METHOD_NOT_ALLOWED`, `415`/`400` per the existing transport rules.

**Synchronous vs asynchronous:** captures last 20–330 ms. Blocking the cooperative superloop
for that long would stall USB and other requests, so `POST` is **asynchronous**: it arms and
returns `202` immediately; the client polls `/status` until `complete`, then fetches `/data`.
This preserves loop responsiveness — the same discipline the benchmark used.

---

## 6. Buffer ownership and transport integration

- A single static buffer lives in the acquisition module:
  `static uint16_t acq_buf[ACQ_MAX_SAMPLES];` (default 32 768 → 64 KB).
- The module exposes read-only access only in `COMPLETE`/`STREAMING`:
  `bool acq_get_view(const void **ptr, size_t *len)` / `void acq_release(void)`.
- **Transport reuse:** `API_CONN_STREAM` currently *generates* the benchmark pattern. Add a
  source mode so a stream can also be backed by a RAM buffer:

  | Source | Used by |
  |--------|---------|
  | `STREAM_SRC_PATTERN` | `GET /benchmark/stream` (unchanged) |
  | `STREAM_SRC_BUFFER` | `GET /acquisition/data` (points at `acq_buf`) |

  For `STREAM_SRC_BUFFER` the pump calls `tcp_write()` directly from `acq_buf + offset` with
  `TCP_WRITE_FLAG_COPY` — no staging copy, and `c->body` stays free for control requests.
- **Single bulk gate:** reuse the existing benchmark stream gate so only **one** bulk binary
  transfer runs at a time (benchmark *or* acquisition data). `DELETE` while `STREAMING` is
  refused; disconnect in `STREAMING` releases the buffer via `acq_release()` (same
  `conn_reset()`/finalize path the benchmark already uses).
- The acquisition module **never** includes lwIP or USB headers — it is pure HAL
  ADC/TIM/DMA + state.

---

## 7. Driver approach (decided after inspection)

The design initially assumed HAL ADC/TIM could simply be enabled. Inspection
found that `Drivers/STM32F4xx_HAL_Driver` is a **trimmed subset** (13 sources:
rcc, flash, gpio, dma, pwr, cortex, exti, hal). There is no
`stm32f4xx_hal_adc.*`, `stm32f4xx_hal_tim.*`, `stm32f4xx_ll_adc.h` or
`stm32f4xx_ll_tim.h` in the repo, so `HAL_ADC_MODULE_ENABLED` /
`HAL_TIM_MODULE_ENABLED` cannot be enabled without vendoring new files.

**Decision: bare-metal ADC + TIM + DMA.** The engine configures ADC1, TIM2 and
DMA2 Stream 0 by register using the complete CMSIS device header
(`stm32f411xe.h`); only `HAL_GPIO_Init` (analog mode) comes from the vendored
HAL. The ADC and DMA interrupt handlers live in the acquisition module. This
keeps the diff small and reviewable, avoids HAL-revision matching, and gives
direct control over the overrun and DMA error flags.

Notes discovered while implementing:

- The CMSIS ADC prescaler field is `ADC_CCR_ADCPRE` (not `PRESC`).
- `ADC_CR2` CAL/RSTCAL bits are absent from this CMSIS revision's defines; they
  are named locally (`ACQ_ADC_CR2_CAL`, `ACQ_ADC_CR2_RSTCAL`) for calibration.
- `ENABLE_ADC` already existed in `hardware_config.h`; it is set to 1 and gates
  `ACQ_Init()` / `ACQ_Task()` from `hardware.c`.
- PA0 is **already** outside the GPIO whitelist; the reservation is made
  explicit in `modules/gpio.c` so a future widening cannot re-admit it.

ADC1 maps to **DMA2 Stream 0, Channel 0** on the F411; TinyUSB device mode uses
no DMA, so there is no conflict with the USB path. Acquisition IRQs run at
priority 6, below USB (priority 5).

---

## 8. Proposed first experiment (answers the four questions)

Scope: **ADC + TIM2-triggered DMA into a 64 KB buffer, software trigger, no pretrigger, no
external trigger.** Just enough to prove the acquisition path.

1. **Stable, known rate?** Configure 1.000 MS/s, capture 32 768 samples, and compare the
   measured wall-clock window against the expected 32.77 ms (e.g. with the `DWT` cycle counter
   or a second timer). Report jitter.
2. **CPU-free DMA?** Increment a heartbeat counter in the superloop during capture; confirm it
   keeps ticking at its normal rate, proving no per-sample CPU involvement.
3. **Overrun detection?** Request an impossible rate (e.g. 2 MS/s) and confirm the ADC sets
   `OVR` and the status reports `overrun:true`.
4. **Streaming without disturbing capture?** `GET /acquisition/data`, verify byte count and a
   deterministic test pattern, and confirm a concurrent `GET /system` still returns `200`
   during the transfer (as the benchmark concurrency test already does).

Deliverables: the engine module, the three routes, `tools/acq_capture.py` (stdlib: trigger,
poll status, fetch, verify length/pattern, print duration and transfer rate), and a tutorial
chapter documenting measured numbers.

---

## 9. Approved decisions (Phase 1)

| # | Decision | Choice |
|---|----------|--------|
| 1 | ADC pin | **PA0 / ADC1_IN0**, excluded from the GPIO whitelist |
| 2 | Default rate | **1.000 MS/s** (timer sets the rate; sample window is the longest that fits) |
| 3 | Buffer | **48 KB / 24 576 samples**, `ACQ_MAX_SAMPLES` (configurable internally) |
| 4 | Sample format | **raw 12-bit codes as `uint16`**, with `scale`/reference metadata |
| 5 | Async model | **asynchronous POST + status polling** |
| 6 | Driver | **bare-metal ADC + TIM + DMA** (see section 7) |

Phase 1 limits: internal timer trigger, single channel, normal-mode DMA, fixed
capture length, no circular DMA, no external trigger, no continuous streaming.

Transfer/capture timing at the default: sample interval 1 µs, 24 576 samples,
capture window **24.576 ms**, transfer **≈ 75.6 ms at 0.65 MB/s**.
