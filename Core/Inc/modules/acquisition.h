//
// Burst acquisition engine (Phase 1).
//
// A timer-triggered ADC capture into a fixed SRAM buffer, driven entirely by
// DMA so the CPU never touches an individual sample. The engine is
// transport-agnostic: it never includes lwIP or USB headers. The REST layer
// only asks for status and, once a capture is COMPLETE, reads the buffer.
//
// Phase 1 scope: single channel (PA0 / ADC1_IN0), internal timer trigger
// (TIM2), normal-mode DMA, fixed capture length. No circular DMA, no external
// trigger, no streaming while capturing.
//

#ifndef USB_WEBSERVER_ACQUISITION_H
#define USB_WEBSERVER_ACQUISITION_H

#include <stdint.h>

/*
 * Sample-buffer capacity, in 16-bit samples. Deliberately a single knob: the
 * architecture is not tied to this value. 24576 samples = 48 KB.
 */
#ifndef ACQ_MAX_SAMPLES
#define ACQ_MAX_SAMPLES 24576u
#endif

/*
 * Metadata for the raw codes. Samples are never converted in the acquisition
 * path; the client applies the scale.
 */
#define ACQ_SAMPLE_BYTES    2u      /* uint16_t on the wire, little-endian */
#define ACQ_RESOLUTION_BITS 12u     /* ADC resolution */
#define ACQ_REFERENCE_MV    3300u   /* nominal VDDA, in millivolts */

/* Rates the engine accepts: must divide the 96 MHz timer clock exactly and be
 * serviceable by the 24 MHz ADCCLK. */
#define ACQ_RATE_MIN_HZ     50000u
#define ACQ_RATE_MAX_HZ     1600000u

typedef enum
{
    ACQ_STATE_IDLE = 0,   /* no capture; buffer free */
    ACQ_STATE_CAPTURING,  /* DMA is filling the buffer */
    ACQ_STATE_COMPLETE,   /* capture done; data valid and stable */
    ACQ_STATE_STREAMING,  /* transport is reading the buffer */
    ACQ_STATE_ERROR       /* capture aborted; data invalid */
} ACQ_State_t;

typedef enum
{
    ACQ_OK = 0,
    ACQ_ERR_DISABLED,     /* engine never initialised (ENABLE_ADC off) */
    ACQ_ERR_PARAM,        /* rate or sample count not supported */
    ACQ_ERR_BUSY,         /* a capture or a read is already active */
    ACQ_ERR_NO_DATA       /* nothing complete to read */
} ACQ_Status_t;

/* One-time hardware bring-up (clocks, PA0 analog, ADC, TIM2, DMA, DWT). */
void ACQ_Init(void);

/* Cooperative-loop hook: watchdog for a capture that never completes. */
void ACQ_Task(void);

/*
 * Arm and immediately trigger a capture of `samples` at `rate_hz`. The capture
 * runs to completion in the background while the loop keeps running.
 *
 * Refused with ACQ_ERR_BUSY unless the state is IDLE or ERROR, so a completed
 * capture is never silently overwritten before the client has read it.
 */
ACQ_Status_t ACQ_Start(uint32_t rate_hz, uint32_t samples);

/* Discard the current capture and return to IDLE. Refused while capturing or
 * streaming (ACQ_ERR_BUSY). */
ACQ_Status_t ACQ_Reset(void);

/* Status snapshot; safe to call from the cooperative loop. */
ACQ_State_t ACQ_GetState(void);
uint32_t    ACQ_GetCaptureId(void);
uint32_t    ACQ_GetCapacity(void);
uint32_t    ACQ_GetRateHz(void);
uint32_t    ACQ_GetCaptured(void);     /* samples captured */
uint32_t    ACQ_GetElapsedUs(void);    /* measured capture window */
uint8_t     ACQ_GetOverrun(void);      /* ADC overrun seen this capture */
uint8_t     ACQ_GetDmaErrors(void);    /* DMA TE/DME/FE errors this capture */
uint8_t     ACQ_GetTimeouts(void);     /* captures aborted by the watchdog */
uint32_t    ACQ_GetTailOverruns(void); /* benign post-burst overruns ignored */

/*
 * Transport view. ACQ_BeginRead pins the buffer and moves COMPLETE->STREAMING;
 * it returns 0 if there is no completed capture. ACQ_EndRead must be called
 * exactly once after a successful ACQ_BeginRead.
 */
uint8_t ACQ_BeginRead(const uint16_t **samples, uint32_t *count);
void    ACQ_EndRead(void);

#endif // USB_WEBSERVER_ACQUISITION_H
