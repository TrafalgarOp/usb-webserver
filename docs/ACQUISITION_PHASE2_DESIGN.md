# Burst Acquisition — Phase 2 Design Proposal: Pre/Post-Trigger Capture

Status: **proposal, for review. Nothing here is implemented.**
Baseline: Phase 1 burst acquisition, frozen at tag `acq-phase1` (`5f0fffc`).
Companion documents: `docs/ACQUISITION_DESIGN.md` (Phase 1 design + bring-up),
`docs/API_TUTORIAL.md` ch. 11 / app. I.

Phase 2 turns the fixed burst into a **triggered** capture: sample continuously
into a ring, keep a configurable number of samples *before* and *after* an event
of interest, then freeze and expose the same `/data` endpoint. The goal is
oscilloscope-like behaviour, not live streaming.

---

## 1. Scope

In scope:

- circular-DMA sampling into the existing 48 KB buffer;
- a software-selectable trigger (immediate / analog watchdog / external edge);
- configurable `pre` and `post` counts;
- deterministic finalisation into a linear buffer, streamed by the unchanged
  `/data` route;
- a bounded wait so an absent trigger cannot wedge the device.

Out of scope (unchanged, deliberately deferred): continuous live streaming,
multi-channel, decimation, on-device mV conversion, lwIP buffer tuning.

**Backward compatibility is a hard requirement.** A `POST` without a `trigger`
object must behave exactly like Phase 1 (normal-mode DMA, immediate, full
buffer). Phase 1 regression tests must still pass byte-for-byte.

---

## 2. The central point: circular DMA is not a trigger

Circular DMA answers "where do samples go", not "when did the event happen".
The DMA writes continuously and never signals completion. A trigger therefore
has to come from somewhere else, and the **quality of the trigger source sets
the quality of the capture**. Three properties matter:

1. **Index accuracy** — how precisely we can name the ring position of the
   triggering sample;
2. **Rate ceiling** — the fastest sampling the source can keep up with;
3. **CPU cost** — whether it competes with USB/lwIP.

The options, with their limits:

| Source | How the event is seen | Index accuracy | Rate ceiling | CPU cost |
|--------|----------------------|----------------|--------------|----------|
| **Software, per-sample** (DMA off, ADC EOC ISR) | CPU reads `DR` and compares each conversion | 1 sample | ~≤ 200 kS/s (to be measured); 1 µs budget at 1 MS/s is too tight alongside USB | high |
| **Software, block poll** (DMA on, loop scans new block) | cooperative loop compares the newest samples | = block size (poor) | tens of kS/s | low but coarse |
| **Analog watchdog** (DMA on, AWD IRQ) | ADC hardware compares every conversion | **±1 sample** (ADC flag + IRQ latency) | **full 1.6 MS/s** | one IRQ per trigger |
| **External digital edge** (DMA on, TIM input capture) | timer latches `CNT` in hardware | **±1 sample, load-independent** | **full** | one IRQ/capture |

Timing basis (from the Phase 1 design): SYSCLK 96 MHz, ADCCLK 24 MHz,
`Tconv = (SMP + 12) / 24 MHz`; at 1 MS/s `Tconv = 0.625 µs` against a 1 µs
period. Cortex-M4 interrupt latency is ~12–20 cycles ≈ 0.13–0.21 µs, i.e.
**0.1–0.2 of a sample period at 1 MS/s**, so the watchdog interrupt lands within
one sample. The external-edge path has no CPU in the path at all.

### 2.1 Recommended trigger: analog watchdog (AWD)

`ADC1` can compare every conversion against `LTR`/`HTR` in hardware
(`ADC_CR1.AWDEN` + `AWDSGL` on channel 0, window in `ADC_HTR`/`ADC_LTR`) and
raise `ADC_SR.AWD`. This is the best fit for Phase 2:

- works at the **full 1.6 MS/s** rate with no per-sample CPU work;
- costs **no extra RAM** and no extra pin;
- accuracy ±1 sample, set only by interrupt latency.

The watchdog is a **level** comparator, not an edge detector. For "crossed a
threshold going up" semantics, set `LTR = level`, `HTR = 0xFFF`, then disarm
after the first hit (single-shot). A genuine edge (rising *through* a
threshold) is not directly available from AWD; that is what the external path
is for.

### 2.2 Optional trigger: external digital edge

If/when a real synchronising edge is needed, route the pin to a spare timer
input capture — e.g. **PA1 = TIM2_CH2**, so the trigger is latched by the same
timer that sets the sample rate, with zero CPU latency. `CCR` gives the
sub-period phase; combined with a lap counter this names the triggering sample
to ±1 sample regardless of load. Cost: one reserved pin (added to the PA0-style
reservation so the GPIO API cannot grab it) and one TIM2 channel.

### 2.3 Not recommended for Phase 2

- **Software per-sample** trigger: viable only at low rates and it removes DMA
  from the hot path, which is a different engine, not a trigger mode. Keep as a
  documented low-rate fallback at most.
- **Comparator (COMP1/2)**: gives a true analog edge, but its output is not
  routable to TIM2 input capture on this package; would need TIM1/8 break or
  EXTI. Revisit only if AWD proves too coarse.

---

## 3. Ring architecture

```
        TIM2 TRGO ──▶ ADC1 ──▶ DMA2 Stream0 (CIRCULAR) ──▶ acq_buffer[N]
                                   │                          ▲
                        ┌──────────┴───────────┐              │
                        │  write index w = N - NDTR          │  AWD IRQ:
                        │  (0..N-1)                          │   i_trig = w
                        └──────────────────────┘             │
   keep window  =  [ i_trig - pre , i_trig + post )  (mod N)  │
```

- `N = ACQ_MAX_SAMPLES = 24576` (48 KB), the **same buffer** Phase 1 uses. No
  second big allocation.
- The DMA runs in circular mode (`DMA_SxCR.CIRC`); `NDTR` reloads to `N` each
  lap, so the write index is exactly `w = N - NDTR`.
- A lap counter, incremented on the DMA transfer-complete interrupt (or derived
  from DWT), gives a monotonic sample count for `elapsed` and for the "have we
  collected `post` samples yet" test.

### 3.1 Pre-trigger retention — it is free

Pre-trigger samples need no special handling: **they are simply the samples
already in the ring when the trigger arrives**. On the trigger we record
`i_trig = w`; the pre-trigger data is `[i_trig - pre, i_trig)`. Retention is
guaranteed as long as the writer does not lap back onto `i_trig - pre`, which is
the margin condition in §3.3.

One consequence: **a trigger that fires before `pre` samples exist has no
complete pre-trigger.** Oscilloscopes keep waiting for a full pre-fill, and so
should we: arm the watchdog only after a warm-up of `pre` samples (a short
delay of `pre/rate`, e.g. 12.3 ms for `pre = 12288` at 1 MS/s). Triggers seen
during warm-up are ignored.

### 3.2 Trigger position is configurable

`pre` and `post` are request parameters. Requested `samples` becomes an alias
for `pre + post` (total kept). Typical presets: `pre = N/2, post = N/4` (centre
trigger), `pre = 0` (post only), `pre = 3N/4` (deep pre-trigger).

### 3.3 Stopping after `post` samples

Circular DMA never ends, so the stop must be produced. The write index advances
from `i_trig`; the oldest sample of the window, `i_trig - pre`, is overwritten
after `N - pre` more samples. We must therefore stop somewhere in the window
`(post, N - pre)` samples after the trigger, and we keep `pre + post`. The
**stop margin** is `N - pre - post` samples; require it to be comfortably
positive (proposal: `ACQ_STOP_MARGIN = 256` samples).

Because the margin absorbs several samples of slop, the stop does **not** need
to be sample-exact — only `i_trig` must be accurate. Candidate stop
mechanisms, cheapest first:

1. **Delayed-stop helper timer (recommended for 2a).** In the AWD ISR, record
   `i_trig` and start a one-pulse helper timer (e.g. TIM3, 96 MHz, `ARR =
   post-1`). Its update interrupt stops TIM2 + DMA and finalises. Stop slop is
   the timer-IRQ latency, ±1–2 samples, absorbed by the margin. One extra timer,
   no extra RAM.
2. **Hardware gate (cleanest).** Put TIM2 in gated slave mode with TIM3 TRGO as
   the gate; arming TIM3 in the trigger ISR lets the hardware stop the sample
   clock itself, with no interrupt in the stop path. More configuration, and it
   reinterprets the Phase 1 TIM2 setup — worth doing only if (1) shows visible
   jitter.
3. **Reprogramming `NDTR` at the trigger** — rejected: `NDTR` is only writable
   while the stream is disabled, and a mid-capture disable drops samples right
   at the trigger, which is the worst place to lose them.

### 3.4 Freezing into a linear buffer (no second buffer)

After the stop, the valid window `[i_trig - pre, i_trig + post)` may wrap. To
keep the transport untouched, **rotate it in place** so it starts at index 0:

- three-reversal rotate: O(1) extra memory, touches each of the `pre + post`
  elements twice;
- cost: ≈ 2 · 24576 · 2 B of moves ≈ 50 k cycles ≈ **< 1 ms**, done *after*
  sampling stops, so it never touches the real-time path;
- the `/data` route then streams `acq_buffer[0 .. pre+post)` exactly as today.

Alternative (not chosen): extend `API_Stream_Src_t` to emit two ranges, which
avoids the rotate but touches `api_server.c`. Rotation is lower-risk for 2a.

### 3.5 Timeout and fault handling

- **No trigger:** from `armed`, if `trigger_timeout_ms` elapses (proposal:
  default 1000 ms, range 10–10000) abort to `ERROR`, stop TIM2/DMA, bump a
  counter. This replaces "waiting forever" with a visible, recoverable state.
- **Overrun / DMA error:** the Phase 1 rules extend unchanged and stay fatal in
  `ARMED` or `CAPTURING` (a stalled ring loses pre-trigger data). The tail-overrun
  exemption is Phase 1-specific and does not apply to a circular burst.
- The finaliser stays the single point that claims the state under
  `__disable_irq()`, so a late watchdog/overrun/DMA event cannot double-transition.

---

## 4. RAM budget (the 77.4 % constraint)

Phase 1 RAM is **101 464 B / 128 KB (77.4 %)**; `acq_buffer` is 48 KB of that.
Phase 2 **adds no large buffer**:

| Item | Bytes |
|------|-------|
| `acq_buffer[24576]` (existing, reused as the ring) | 49 152 (already counted) |
| new state: `i_trig`, `pre`, `post`, `trigger_*`, lap counter, flags | ≈ 40 |
| helper-timer setup | 0 (peripheral registers) |
| rotate scratch | 0 (in-place) |
| **net new `.bss`/`.data`** | **≈ 40 B** |

A second 48 KB staging buffer would take RAM to ~124 KB (~97 %) and is
explicitly rejected. The proposal is to **keep the ring in the existing
buffer**, and to expose `pre`/`post`/capacity so the client can size the split
without the firmware allocating anything.

If a future feature genuinely needs two buffers, the honest options are: shrink
`ACQ_MAX_SAMPLES` (e.g. 24 KB + 24 KB) or move the ring to CCM/SRAM2 — both are
their own decisions, not Phase 2.

---

## 5. Timing budget (proposed, to be measured on approval)

| Quantity | Value | Notes |
|----------|-------|-------|
| Rate range | 50 kS/s – 1.6 MS/s | unchanged from Phase 1 |
| AWD index error | ±1 sample | ADC flag + IRQ latency 0.13–0.21 µs vs 0.625 µs period |
| External-edge index error | ±1 sample | hardware latch, load-independent |
| Stop slop (helper timer) | ±1–2 samples | absorbed by `ACQ_STOP_MARGIN` |
| Warm-up before accept | `pre` samples | e.g. 12.3 ms at 1 MS/s, `pre = 12288` |
| Pre-trigger window | `pre / rate` | 12.288 ms at 1 MS/s, `pre = 12288` |
| Post-trigger window | `post / rate` | 8.192 ms at 1 MS/s, `post = 8192` |
| Finalise rotate | < 1 ms | off the real-time path |
| Transfer | ≈ `2·(pre+post)` B at 0.66 MB/s | same contract as Phase 1 |
| Trigger timeout | 1000 ms default | bounded wait |

Worst-case total wait for a client that arms then polls: warm-up + (time to
trigger) + post/rate + rotate + transfer.

---

## 6. API and state-machine changes (proposed)

### 6.1 Request (backward compatible)

```
POST /api/v1/acquisition/capture
{
  "rate_hz": 1000000,
  "trigger": {
    "source": "analog_watchdog",   // analog_watchdog | external | immediate
    "level": 2048,                 // AWD: rising => value > level  (0..4095)
    "edge": "rising",              // rising | falling
    "pre": 12288,
    "post": 8192                   // total kept = pre + post
  },
  "trigger_timeout_ms": 1000
}
```

- `trigger` absent ⇒ **Phase 1 behaviour** (`immediate`, full buffer).
- `source: "immediate"` is the explicit form of the same thing.
- `external` adds `"pin": "PA1"` and `"edge"`.
- Validation → `400 INVALID_VALUE`: `pre + post > capacity - ACQ_STOP_MARGIN`;
  `level` out of range; unknown `source`; bad pin; `trigger_timeout_ms` out of
  range.

### 6.2 Status additions

`armed`, `triggered`, `trigger_source`, `trigger_index`, `pre`, `post`,
`waited_us`, `trigger_timeouts` — alongside the existing Phase 1 fields.

### 6.3 State machine

```
idle ──POST(immediate)──▶ capturing ──done──▶ complete ──GET /data──▶ streaming
  ▲                            ▲                  │                      │
  │                            │                  └────── DELETE ────────┘
  │  POST(triggered)           │
  └──▶ armed ──AWD/edge──▶ capturing ──post done──▶ complete
        │
        └── timeout ──▶ error ──DELETE──▶ idle
```

`armed` is visible in `/status` so the client can distinguish "waiting for
trigger" from "sampling". Ownership is **unchanged**: `complete`/`streaming`
still refuses a new `POST` (`409 ACQUISITION_BUSY`), the transport still pins
the ring only after taking the single-session bulk gate, and `ACQ_BeginRead`
still moves `COMPLETE → STREAMING`.

---

## 7. Interaction with Phase 1 (regression risks)

| Risk | Mitigation |
|------|-----------|
| AWD registers disturb the normal path | AWD disabled (`AWDEN=0`) for `immediate`; feature is opt-in per capture |
| Circular DMA vs Phase 1 `DDS=1` | Re-verify on hardware: in circular mode there is no "last transfer", so `DDS` semantics differ from Phase 1 — a bring-up item, not an assumption |
| New stop-timer IRQ priority | Keep below USB (5); order DMA(6) > ADC(7) > helper(8) so the finaliser is never raced |
| GPIO API grabbing the trigger pin | Add the pin to the PA0-style reservation in `modules/gpio.c` |
| `ADCCLK`/sample-window planning | Unchanged; the window still only needs to fit one period |
| Benchmark transport | Untouched; re-run the frozen benchmark after Phase 2 |

---

## 8. Incremental plan (on approval)

Matching the agreed sequence:

- **2a — ring + AWD + fixed pre/post.** Circular DMA, AWD trigger, hard-coded
  `pre`/`post`, helper-timer stop, in-place rotate, same `/data`. Verify index
  accuracy, no data loss, RAM ≈ unchanged, Phase 1 still passes.
- **2b — configurability + timeout.** Request/status fields, `armed` state,
  trigger timeout, validation. Full error-surface tests.
- **2c — external digital edge.** TIM2 input capture, pin reservation, edge
  semantics. Only if 2a/2b show AWD is not enough.
- Deferred: continuous live streaming.

### 8.1 Test plan without lab equipment

The board has no signal generator, so Phase 2 needs a repeatable source:

- **AWD self-test via an internal channel.** Point the ADC at an internal,
  near-constant channel (`Vrefint` IN17 or temperature IN16) and set the AWD
  level just below/above its known code. That makes the trigger fire
  deterministically every capture with **no wiring**, proving index accuracy,
  pre/post split and timeout behaviour.
- **PWM → RC → PA0.** A spare timer channel (e.g. PA6 = TIM3_CH1) driving an RC
  gives a known ramp for realistic AWD/edge tests; the same pin can feed an
  external-trigger input for 2c.
- **Phase 1 regression.** Re-run `tools/acq_capture.py --repeats`, `--concurrent`,
  the ownership/error suite, and the benchmark; all must match the frozen
  numbers.

---

## 9. Decisions requested before implementation

1. **Trigger source for 2a** — analog watchdog (recommended) / external edge /
   both.
2. **Stop mechanism for 2a** — delayed-stop helper timer (recommended) /
   hardware gate.
3. **Default split** — `pre = N/2`, `post = N/4` (recommended) or another.
4. **`ACQ_STOP_MARGIN`** — 256 samples (recommended).
5. **Trigger timeout default** — 1000 ms (recommended).
6. **`samples` semantics** — alias of `pre + post` (recommended) / keep as
   capacity.
7. **Freeze tag** — branch `acquisition-trigger` off `acq-phase1`; tag the result
   `acq-phase2` on success.
