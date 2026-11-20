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

The watchdog flags a conversion whose result is **outside** the window
`[LTR, HTR]` — i.e. `< LTR` or `> HTR` — so the window encodes the direction:

| Wanted condition | Window | `LTR` | `HTR` |
|------------------|--------|-------|-------|
| rising: first code **above** `level` | `[0, level]` | 0 | `level` |
| falling: first code **below** `level` | `[level, 4095]` | `level` | 4095 |

A code exactly equal to `level` is *inside* the window and does not trigger, so
the semantics are strict (`> level` / `< level`). (An earlier draft of this
document had these two rows swapped — the window, not the edge field, selects
the direction.)

The watchdog is a **level** comparator, not an edge detector: it keeps flagging
for as long as the input stays out of window. It must therefore be **disarmed on
the first hit** — clear `AWDIE` and `AWDEN` in the ISR, then clear the status
flag (`ADC_SR.AWD` is `rc_w0`) — so the ring can keep filling without a storm of
watchdog interrupts. A true *edge through* a threshold is not directly available
from AWD; that is what the external path is for.

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

### 2.4 Deriving the trigger sample index (hypothesis, to be verified)

`ADC_SR.AWD` marks when the watchdog **condition** was met, not which ring slot
holds the offending sample. The index has to be reconstructed from the DMA
write pointer, which trails the analog event through a short pipeline:

```text
analog sample ──▶ SAR conversion ──▶ DR write ──▶ DMA transfer ──▶ NDTR--
                    (Tconv)            (~ns AHB)     (~ns)
```

At the AWD interrupt, `w = N - NDTR` names the *last sample the DMA has finished
writing*. If further conversions completed before the ISR ran, `w` is already
one or more samples past the offending one:

```text
i_trig = w - k       k = samples completed between the AWD event and the read
```

The AWD interrupt and the DMA write for the *same* conversion are independent
hardware paths, so either can be observed first: the ISR may run before that
conversion's ring slot is updated, or after. `k` is therefore a **signed,
modular offset**, not an unsigned difference, and the probe must record it as
such rather than assume an order.

`k` is expected to be 0 or 1 at 1 MS/s but is **not proven**. Therefore:

- the "±1 sample" figure is a hypothesis, not a specification;
- it must be **measured at several rates** — 50 kS/s, 500 kS/s, 1 MS/s,
  1.6 MS/s — because `k` scales with `latency / sample-period` and the period
  shrinks as the rate rises;
- if `k` is unstable, options are (a) read `NDTR` as the very first action of
  the ISR, before any other work; (b) subtract a calibrated constant that may be
  rate-dependent; or (c) timestamp with a second channel (external edge, §2.2),
  which removes the CPU from the path entirely.

The probe in §10.1 is what makes this measurable, at each rate and under load.

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

The request carries the total count `samples` and the pre-trigger count
`trigger.pre`; `post = samples - pre` is derived. The capture length is
`samples = pre + post`, deliberately *not* the ring size `N`:
`N = ACQ_MAX_SAMPLES` is only the physical ring.

Constraints (all → `400 INVALID_VALUE` otherwise):

- `1 <= post` (a triggered capture needs at least one post-trigger sample, so
  `pre == samples` is rejected);
- `0 <= pre < samples`;
- `samples <= N - ACQ_STOP_MARGIN` (§3.3), i.e. the stop margin is **reserved
  out of the ring**, not left implicit. With `N = 24576` and the initial margin
  of 256, the effective maximum is **24320 samples**.

Typical presets: `pre = N/2, post = N/4` (centre trigger), `pre = 0` (post
only), `pre = 3N/4` (deep pre-trigger).

### 3.3 Stopping after `post` samples

Circular DMA never ends, so the stop has to be produced. The write index
advances from `i_trig`; the oldest sample of the window, `i_trig - pre`, is
overwritten after `N - pre` more samples. So the stop must land somewhere in
`(post, N - pre)` samples after the trigger, and the **stop margin** is

```text
ACQ_STOP_MARGIN = (N - pre) - post = N - samples     [samples]
```

This is ring **capacity, not slack**: §3.2 reserves it out of the ring. Its size
must cover the worst-case delay of the whole stop path, so the pre-trigger
window is never overwritten. Proposal: start at 256 and confirm experimentally
(§10.2).

Because the margin absorbs slop, the stop does **not** need to be sample-exact
— only `i_trig` must be accurate. Candidate stop mechanisms, cheapest first:

1. **Delayed-stop helper timer (recommended for 2a).** TIM3, one-pulse, armed
   in the AWD ISR. To make one tick equal one sample period it must be
   **prescaled to the sample rate**, not left at 96 MHz:

   ```text
   TIM3 PSC = 96_000_000 / rate_hz - 1     (rates divide 96 MHz exactly)
   TIM3 ARR = post - 1
   → update interrupt after `post` sample periods
   ```

   e.g. 1 MS/s → PSC 95; 1.6 MS/s → PSC 59; 500 kS/s → PSC 191. `post` fits the
   16-bit `ARR` (`post <= 24320`). Set `URS` so arming emits no spurious update.

   **Latency caveat (central, not a detail):** the timer starts in the AWD ISR,
   not at the offending conversion, so it begins `k` samples late (§2.4).
   Sampling therefore actually stops `post + k + slop` samples after `i_trig`,
   where `slop` is the stop-ISR latency. Extraction always takes exactly
   `[i_trig - pre, i_trig + post)`, so stopping *late* is harmless **provided the
   margin covers `k + slop`**, while stopping *early* would truncate `post`.
   `ARR = post - 1` is therefore safe by construction (it can only overshoot),
   but the margin must be sized from the **measured** `k`. Subtracting `k` from
   `ARR` (compensation) is optional and only tightens the stop point. Do **not**
   assume `ARR = post - 1` yields exactly `post` samples after the trigger:
   TIM3 is not phase-locked to TIM2 either, so even the nominal count is
   `post ± 1`. Both uncertainties are absorbed by the margin.

2. **Hardware gate (cleanest).** Put TIM2 in gated slave mode with TIM3 TRGO as
   the gate; arming TIM3 in the trigger ISR lets the hardware stop the sample
   clock itself, with no interrupt in the stop path. More configuration, and it
   reinterprets the Phase 1 TIM2 setup — worth doing only if (1) shows visible
   jitter.

3. **Reprogramming `NDTR` at the trigger** — rejected: `NDTR` is only writable
   while the stream is disabled, and a mid-capture disable drops samples right
   at the trigger, which is the worst place to lose them.

**The trigger timeout does not use TIM3.** A 1 kHz tick is *not* reachable from
96 MHz within the 16-bit prescaler — the slowest tick is
`96 MHz / 65536 ≈ 1464.8 Hz` — so a "1 kHz / `ARR = 999`" form is not
implementable as written. The timeout is handled in the cooperative loop exactly
like the Phase 1 capture watchdog: `ACQ_Task` compares `HAL_GetTick()` against
`trigger_timeout_ms` while in `ARMED`. If a hardware timeout were ever preferred,
use `PSC = 65535` (tick ≈ 1.465 kHz) with `ARR ≈ trigger_timeout_ms · 1.465`.

### 3.4 Freezing into a linear buffer (no second buffer)

After the stop, the valid window `[i_trig - pre, i_trig + post)` may wrap. To
keep the transport untouched, **rotate it in place** so it starts at index 0.
The rotate is only allowed to run when **all** of the following hold:

- **DMA and the trigger timer are stopped** — the sampling path is quiescent;
- **the transport does not own the buffer** — the rotate happens inside the
  `CAPTURING → COMPLETE` transition, before `ACQ_BeginRead` can pin it, so a
  stream can never observe a half-rotated buffer;
- it produces **exactly `pre + post` samples in chronological order**, starting
  at the oldest pre-trigger sample;
- it uses **bounded, tested operations and no second full-size buffer** —
  three-reversal rotate, O(1) extra memory, each element touched twice,
  ≈ 2 · `samples` · 2 B of moves ≈ 1 ms worst case, all off the real-time path.

The `/data` route then streams `acq_buffer[0 .. pre+post)` exactly as today, so
`api_server.c` and the streaming contract are unchanged.

Alternative (not chosen): extend `API_Stream_Src_t` to emit two ranges (natural
segment then wrapped segment), which avoids the move but adds a new code path to
the already-tested data endpoint. In-place rotation is the lower-risk choice for
2a.

### 3.5 Timeout and fault handling

- **No trigger:** from `ARMED`, if `trigger_timeout_ms` elapses (default
  1000 ms, range 10–10000) abort to `ERROR`, stop TIM2/DMA/TIM3, and set a
  distinct fault reason. The state carries **`TRIGGER_TIMEOUT`** so a client can
  tell an absent trigger apart from a user-requested discard
  (`DELETE` → `IDLE`). `DELETE` must still work from `ERROR` and return the
  engine to `IDLE`.
- **Overrun / DMA error:** the Phase 1 rules extend and stay fatal in `ARMED` or
  `CAPTURING` (a stalled ring loses pre-trigger data). The Phase 1 tail-overrun
  exemption is specific to a *finite* burst and does **not** apply to the
  circular ring.
- The finaliser stays the single point that claims the state under
  `__disable_irq()`, so a late watchdog/overrun/DMA/stop event cannot
  double-transition.

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
| Max capture length | `N - ACQ_STOP_MARGIN` = 24320 | margin reserved out of the ring |
| AWD index error (`k`) | **unknown (hypothesis ≤ 1 sample)** | §2.4 / §10.1: measure per rate and under load |
| External-edge index error | ±1 sample | hardware latch, load-independent |
| TIM3 helper prescaler | `96e6 / rate_hz - 1` | one helper tick = one sample period |
| Stop overshoot | `post + k + slop` after `i_trig` | always ≥ `post`; margin must cover `k + slop` |
| Warm-up before accept | `pre` samples | e.g. 12.3 ms at 1 MS/s, `pre = 12288` |
| Pre-trigger window | `pre / rate` | 12.288 ms at 1 MS/s, `pre = 12288` |
| Post-trigger window | `post / rate` | 8.192 ms at 1 MS/s, `post = 8192` |
| Finalise rotate | ≈ 1 ms | off the real-time path |
| Transfer | ≈ `2·samples` B at 0.66 MB/s | same contract as Phase 1 |
| Trigger timeout | 1000 ms default | `ACQ_Task` + `HAL_GetTick`, not TIM3 |

Worst-case total wait for a client that arms then polls: warm-up + (time to
trigger) + post/rate + rotate + transfer.

---

## 6. API and state-machine changes (proposed)

### 6.1 Request (backward compatible)

```json
POST /api/v1/acquisition/capture
{
  "rate_hz": 1000000,
  "samples": 20480,                // total capture length = pre + post
  "trigger": {
    "source": "analog_watchdog",   // analog_watchdog | external | immediate
    "level": 2048,                 // 0..4095
    "edge": "rising",              // rising | falling
    "pre": 12288                   // pre-trigger count; post = samples - pre
  },
  "trigger_timeout_ms": 1000
}
```

`post` is **derived**: `post = samples - pre` (here 8192). `samples` is the total
kept, **not** the ring size `N`.

- `trigger` absent ⇒ **Phase 1 behaviour unchanged** — normal-mode DMA, full
  buffer, `samples` used exactly as today. Existing requests are never silently
  reinterpreted.
- `source: "immediate"` is the explicit form of the same thing.
- `external` adds `"pin": "PA1"` (reserved, §7).

Validation → `400 INVALID_VALUE`:

- `pre > samples` (would make `post` negative);
- `pre == samples` (zero post-trigger samples; not supported in 2a — a triggered
  capture needs at least one);
- `samples > N - ACQ_STOP_MARGIN` (the margin reserves ring capacity);
- `samples == 0`; `level` outside `0..4095`; unknown `source`; bad `pin`;
  `trigger_timeout_ms` outside `10..10000`.

### 6.2 Status additions

Alongside the existing Phase 1 fields: `armed`, `triggered`, `trigger_source`,
`trigger_index`, `pre`, `post`, `waited_us`, `trigger_timeouts`, and a **fault
reason** (present in `error` only) that distinguishes `TRIGGER_TIMEOUT` from an
overrun/DMA fault. `DELETE` clears it and returns to `idle`.

### 6.3 State machine

```
idle ──POST(immediate)──▶ capturing ──done──▶ complete ──GET /data──▶ streaming
  ▲                            ▲                  │                      │
  │                            │                  └────── DELETE ────────┘
  │  POST(triggered)           │
  └──▶ armed ──AWD/edge──▶ capturing ──post done──▶ complete
        │
        └── TRIGGER_TIMEOUT ──▶ error ──DELETE──▶ idle
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

- **2.0 — trigger-index probe (step 0, before the engine).** Throwaway firmware
  that measures `Δ` between the AWD interrupt and the true trigger slot (§10.1)
  across rates and under load. No state machine, no API. This gates 2a.
- **2a — ring + AWD + fixed pre/post.** Circular DMA, AWD trigger, hard-coded
  `pre`/`post`, helper-timer stop, in-place rotate, same `/data`. Verify index
  accuracy, no data loss, RAM ≈ unchanged, Phase 1 still passes.
- **2b — configurability + timeout.** Request/status fields, `armed` state,
  trigger timeout, validation. Full error-surface tests.
- **2c — external digital edge.** TIM2 input capture, pin reservation, edge
  semantics. Only if 2a/2b show AWD is not enough.
- Deferred: continuous live streaming.

### 8.1 Test plan without lab equipment

The board has no signal generator, so Phase 2 needs a repeatable source. Two
constraints shape the choice:

- **`Vrefint` needs a long sample window.** Its minimum sampling time is
  ~10 µs — an SMP window of 480 cycles at 24 MHz ADCCLK — which only fits sample
  periods ≥ ~20.5 µs, i.e. **rate ≤ ~48 kS/s**. It is usable as the
  *lowest-rate* point only, not across the sweep.
- **A constant out-of-window input cannot identify the trigger slot.** If every
  sample violates the window, the "first qualifying conversion" is unknowable
  from the ring alone. Identifying the index needs a **unique, single crossing
  inside the captured window**.

Concrete aids:

- **Software-generated step (primary).** A GPIO output (e.g. PB1) through a
  small series resistor into PA0, with a small capacitor to ground, forms an RC
  edge. Toggle the GPIO at a known DMA position; the edge crosses `level` once,
  so the ring itself reveals the true trigger sample — the reference point the
  probe needs. Low output impedance keeps the sample window short, so this works
  at **every** rate.
- **`Vrefint` (low-rate point only).** A no-wiring sanity check at 50 kS/s and
  for pipeline/latency calibration, with the caveat above.
- **PWM → RC → PA0.** A spare timer channel (e.g. PA6 = TIM3_CH1) driving an RC
  gives a periodic ramp for realistic AWD tests; the same pin can feed an
  external-trigger input for 2c.
- **Phase 1 regression.** Re-run `tools/acq_capture.py --repeats`, `--concurrent`,
  the ownership/error suite, and the benchmark; all must match the frozen
  numbers.

---

## 9. Decisions recorded (approved for design; implementation still gated)

1. **Trigger source for 2a** — analog watchdog, with direction encoded by the
   window (§2.1), disarmed on first hit. External edge stays a later extension
   (2c).
2. **Stop mechanism for 2a** — delayed-stop helper timer, **prescaled to the
   sample rate** (`PSC = 96e6 / rate_hz - 1`, `ARR = post - 1`), not 96 MHz
   directly. The stop may overshoot `post` by `k + slop`; approval is
   conditional on the margin covering that, measured under load (§10.2). The
   hardware gate (§3.3 option 2) remains the fallback.
3. **Default split** — `pre = N/2`, `post = N/4`. The **total capture length is
   `samples = pre + post`, not `N`**; `N` is only the physical ring.
4. **`ACQ_STOP_MARGIN`** — 256 samples as an *initial* value, not proven. It
   reserves ring capacity and must be validated experimentally (§10.2).
5. **Timeout** — 1000 ms default, detected in `ACQ_Task` via `HAL_GetTick`
   (Phase 1 precedent). `ARMED → ERROR` with reason `TRIGGER_TIMEOUT`; `DELETE`
   returns to `IDLE`. A 1 kHz hardware tick is not reachable from 96 MHz (§3.3).
6. **`samples`** — the total requested count (`pre + post`); `post` is derived
   as `samples - pre`. `pre > samples` and `pre == samples` are rejected. A
   request with no `trigger` preserves Phase 1 request behaviour exactly.
7. **Branching** — work on `acquisition-trigger`, based on `acq-phase1`. The
   `acq-phase2` tag is created **only after implementation and hardware
   verification**.

These are recorded, not yet implemented. The reference firmware is unchanged;
§10 lists what must be settled on hardware before the design can be trusted.

---

## 10. Validation items (hardware, before the design is trusted)

### 10.1 Trigger sample index and the ADC/DMA pipeline (step 0)

`ADC_SR.AWD` marks the **condition**, not the slot. The ±1-sample figure is a
hypothesis and must be **measured** — at several rates and under load — before
the engine is written. This is the single highest-risk unknown in Phase 2.

**Probe (throwaway, built before the state machine):**

1. Choose a source that gives a **unique crossing** in the captured window
   (§8.1): a software-generated RC step into PA0 (primary), or `Vrefint` at the
   lowest rate only.
2. Start circular DMA at a known rate and record the DMA write position.
3. Arm AWD with a window whose first crossing is identifiable in the ring; for
   a rising crossing that is `[0, level]` (§2.1).
4. Let the first qualifying conversion raise `ADC_SR.AWD`.
5. In the ISR, **read `NDTR` as the very first action** and record the write
   index, the circular lap count and `ADC_SR`; then disarm the watchdog.
6. Stop DMA after a controlled interval and compare the **observed** DMA index
   with the **true** trigger slot found by scanning the ring for the crossing.

**Method notes:**

- record a **signed, modular offset** `Δ = (observed − true) mod N`, not an
  unsigned subtraction — the AWD interrupt and the DMA write for the *same*
  conversion may be observed in either order, and that ordering is itself the
  quantity being measured;
- repeat at **50 kS/s, 500 kS/s, 1 MS/s, 1.6 MS/s**, both idle and under
  USB/lwIP load, and report the distribution of `Δ` per rate;
- feed the measured `k` into the margin (§10.2) and, if desired, into the TIM3
  `ARR` compensation (§3.3).

If `Δ` proves unstable, apply the §2.4 fallbacks (read `NDTR` first; per-rate
calibration; or move to the external-edge path).

### 10.2 Stop margin under worst-case interrupt latency

`ACQ_STOP_MARGIN = 256` is a starting value. It must be shown to hold when the
stop interrupt is delayed by concurrent USB/lwIP activity — i.e. the worst-case
stop slop, in samples, must stay below the margin so the pre-trigger window is
never overwritten. Measure **under load**, not on an idle bus.

### 10.3 Circular DMA vs `DDS`

In circular mode there is no "last transfer", so Phase 1's `DDS=1` rationale
does not carry over unchanged. Verify request generation holds across laps.

### 10.4 Phase 1 regression

`tools/acq_capture.py --repeats`, `--concurrent`, the ownership/error suite and
the benchmark must all still match the frozen `acq-phase1` numbers.
