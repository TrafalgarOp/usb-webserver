//
// Burst acquisition engine (Phase 1): bare-metal ADC + TIM + DMA.
//
// The STM32F4xx HAL in this project is a trimmed subset with no ADC or TIM
// module, so the engine drives ADC1, TIM2 and DMA2 Stream 0 by register,
// using the CMSIS device definitions. Only the GPIO helper (analog mode) comes
// from the vendored HAL. Timing truth is measured with the DWT cycle counter.
//

#include "main.h"
#include "hardware_config.h"
#include "modules/acquisition.h"

/*
 * ADC_CR2 calibration bits are present in hardware on every STM32F4 but are
 * omitted from this CMSIS revision's ADC_CR2 defines, so name them here.
 */
#define ACQ_ADC_CR2_CAL    (1UL << 2)
#define ACQ_ADC_CR2_RSTCAL (1UL << 3)

/* ------------------------------------------------------------------------- */
/* Capture planning                                                           */
/* ------------------------------------------------------------------------- */

/*
 * The ADC sample-time field, indexed by the SMPR encoding. Total conversion
 * time is (sample_cycles + 12) ADC clocks for a 12-bit conversion.
 */
static const uint16_t ACQ_SMP_CYCLES[8] = { 3, 15, 28, 56, 84, 112, 144, 480 };

/*
 * Resolve a sampling rate to timer geometry and the longest ADC sample window
 * that still fits one trigger period. Returns 0 if the rate is unsupported.
 *
 * Timer (TIM2) runs at 96 MHz, ADC (ADCCLK) at 24 MHz (PCLK2 / 4), so one
 * timer tick is a quarter of an ADC clock. A sample window fits when
 * (sample_cycles + 12) * 4 <= ticks.
 */
static uint8_t acq_plan(uint32_t rate_hz,
                        uint16_t *psc, uint16_t *arr, uint8_t *smp_field)
{
    uint32_t ticks;
    uint32_t i;
    uint8_t  best = 0xFFu;

    if (rate_hz < ACQ_RATE_MIN_HZ || rate_hz > ACQ_RATE_MAX_HZ)
    {
        return 0;
    }

    /* Require an exactly divisible timer period so the rate is precise. */
    if ((96000000u % rate_hz) != 0u)
    {
        return 0;
    }

    ticks = 96000000u / rate_hz;
    if (ticks < 1u || ticks > 65536u)
    {
        return 0;
    }

    for (i = 0; i < 8u; i++)
    {
        if (((uint32_t)ACQ_SMP_CYCLES[i] + 12u) * 4u <= ticks)
        {
            best = (uint8_t)i;   /* last (longest) fit wins */
        }
    }

    if (best == 0xFFu)
    {
        return 0;   /* even the 3-cycle window is too slow */
    }

    *psc = 0;                          /* 96 MHz / 1 = 96 MHz counter */
    *arr = (uint16_t)(ticks - 1u);
    *smp_field = best;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static uint16_t acq_buffer[ACQ_MAX_SAMPLES] __attribute__((aligned(4)));

static volatile ACQ_State_t acq_state = ACQ_STATE_IDLE;
static volatile uint8_t acq_overrun = 0;
static volatile uint8_t acq_dma_errors = 0;

static uint32_t acq_capture_id = 0;
static uint32_t acq_rate_hz = 0;
static uint32_t acq_samples = 0;      /* requested sample count */
static uint32_t acq_captured = 0;     /* samples actually captured */
static uint32_t acq_elapsed_us = 0;   /* measured capture window */
static uint32_t acq_start_cycles = 0;
static uint32_t acq_start_tick = 0;
static uint8_t  acq_ready = 0;        /* set once ACQ_Init has run */
static uint8_t  acq_dwt_ok = 0;       /* DWT cycle counter is counting */

/* ------------------------------------------------------------------------- */
/* Hardware helpers                                                          */
/* ------------------------------------------------------------------------- */

/* Stop the trigger source and the DMA stream, in that order. The ADC stays
 * powered (ADON) so it is never re-calibrated; with no trigger and the DMA
 * off it produces no conversions. */
static void acq_stop_hw(void)
{
    volatile uint32_t guard;

    TIM2->CR1 &= ~TIM_CR1_CEN;

    DMA2_Stream0->CR &= ~DMA_SxCR_EN;
    for (guard = 0; guard < 100000u && (DMA2_Stream0->CR & DMA_SxCR_EN); guard++)
    {
        /* bounded wait: the stream disables on the next AHB cycle */
    }
}

/* Clear every DMA2 Stream 0 flag. */
static void acq_clear_dma_flags(void)
{
    DMA2->LIFCR = DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 |
                  DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0;
}

/*
 * Finalise a capture. Idempotent and safe to call from both the DMA and the
 * ADC interrupt: the first caller claims the CAPTURING state under a critical
 * section, so a late overrun or completion cannot produce a contradictory
 * transition. Called only from interrupt context or from ACQ_Task.
 */
static void acq_finalize(uint8_t error)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();

    if (acq_state != ACQ_STATE_CAPTURING)
    {
        __set_PRIMASK(primask);
        return;
    }

    acq_stop_hw();

    if (error)
    {
        acq_state = ACQ_STATE_ERROR;
    }
    else
    {
        uint32_t left = DMA2_Stream0->NDTR;

        acq_captured = (left <= acq_samples) ? (acq_samples - left) : acq_samples;
        acq_elapsed_us = acq_dwt_ok
            ? ((DWT->CYCCNT - acq_start_cycles) / (SystemCoreClock / 1000000u))
            : ((HAL_GetTick() - acq_start_tick) * 1000u);
        acq_state = ACQ_STATE_COMPLETE;
    }

    __set_PRIMASK(primask);
}

/* ------------------------------------------------------------------------- */
/* Interrupt handlers                                                         */
/* ------------------------------------------------------------------------- */

void DMA2_Stream0_IRQHandler(void)
{
    uint32_t lisr = DMA2->LISR;

    if (lisr & (DMA_LISR_TEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_FEIF0))
    {
        DMA2->LIFCR = DMA_LIFCR_CTEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0;
        acq_dma_errors++;
        acq_finalize(1);
        return;
    }

    if (lisr & DMA_LISR_TCIF0)
    {
        DMA2->LIFCR = DMA_LIFCR_CTCIF0;
        acq_finalize(0);
    }
}

void ADC_IRQHandler(void)
{
    uint32_t sr = ADC1->SR;

    ADC1->SR = 0;   /* all status bits are rc_w0: writing 0 clears them */

    if ((sr & ADC_SR_OVR) && acq_state == ACQ_STATE_CAPTURING)
    {
        acq_overrun = 1;
        acq_finalize(1);
    }
}

/* ------------------------------------------------------------------------- */
/* Initialisation                                                             */
/* ------------------------------------------------------------------------- */

void ACQ_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    volatile uint32_t guard;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    /* PA0 as analog input (ADC1_IN0). Owned by this module, never the GPIO API. */
    gpio.Pin  = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* ADCCLK = PCLK2 / 4 = 24 MHz (the fastest legal prescaler here). */
    ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE_0;

    /* --- DMA2 Stream 0, Channel 0: ADC1->DR -> memory, 16-bit, normal mode. */
    DMA2_Stream0->CR = 0;
    for (guard = 0; guard < 100000u && (DMA2_Stream0->CR & DMA_SxCR_EN); guard++)
    {
    }
    acq_clear_dma_flags();

    DMA2_Stream0->PAR  = (uint32_t)&ADC1->DR;
    DMA2_Stream0->M0AR = (uint32_t)acq_buffer;
    DMA2_Stream0->NDTR = 0;
    DMA2_Stream0->FCR  = DMA_SxFCR_FEIE;   /* FIFO error interrupt (direct mode) */
    DMA2_Stream0->CR   = DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC |
                         DMA_SxCR_PL_1 | DMA_SxCR_TCIE | DMA_SxCR_TEIE |
                         DMA_SxCR_DMEIE;

    /* --- ADC1: 12-bit, single channel 0, TIM2 TRGO rising, DMA, OVR IRQ. --- */
    ADC1->CR1  = ADC_CR1_OVRIE;
    ADC1->CR2  = ADC_CR2_EXTEN_0                     /* rising edge        */
               | ADC_CR2_EXTSEL_1 | ADC_CR2_EXTSEL_2 /* 0110 = TIM2_TRGO    */
               | ADC_CR2_DMA;                        /* DMA mode           */
    ADC1->SQR1 = 0;                                  /* one conversion     */
    ADC1->SQR2 = 0;
    ADC1->SQR3 = 0;                                  /* SQ1 = channel 0    */
    ADC1->SMPR1 = 0;
    ADC1->SMPR2 = 0;

    /* Power up and calibrate (one-shot), with bounded spins. */
    ADC1->CR2 |= ADC_CR2_ADON;
    for (guard = 0; guard < 1000u; guard++)
    {
    }
    ADC1->CR2 |= ACQ_ADC_CR2_RSTCAL;
    for (guard = 0; guard < 100000u && (ADC1->CR2 & ACQ_ADC_CR2_RSTCAL); guard++)
    {
    }
    ADC1->CR2 |= ACQ_ADC_CR2_CAL;
    for (guard = 0; guard < 100000u && (ADC1->CR2 & ACQ_ADC_CR2_CAL); guard++)
    {
    }

    /* --- TIM2: TRGO = update event; period programmed per capture. --- */
    TIM2->CR1 = 0;
    TIM2->CR2 = TIM_CR2_MMS_1;   /* MMS = 010 (update) */
    TIM2->PSC = 0;
    TIM2->ARR = 0;
    TIM2->CNT = 0;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR  = 0;

    /* --- Interrupts: below USB (priority 5) so USB is never delayed. --- */
    NVIC_SetPriority(DMA2_Stream0_IRQn, 6);
    NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    NVIC_SetPriority(ADC_IRQn, 6);
    NVIC_EnableIRQ(ADC_IRQn);

    /* --- DWT cycle counter for a microsecond-accurate capture window. --- */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    {
        uint32_t a = DWT->CYCCNT;

        for (guard = 0; guard < 200u; guard++)
        {
        }
        acq_dwt_ok = (DWT->CYCCNT != a) ? 1u : 0u;
    }

    acq_state = ACQ_STATE_IDLE;
    acq_ready = 1;
}

/* ------------------------------------------------------------------------- */
/* Control                                                                    */
/* ------------------------------------------------------------------------- */

ACQ_Status_t ACQ_Start(uint32_t rate_hz, uint32_t samples)
{
    uint16_t psc;
    uint16_t arr;
    uint8_t  smp;
    uint32_t primask;

    if (!acq_ready)
    {
        return ACQ_ERR_DISABLED;
    }

    if (!acq_plan(rate_hz, &psc, &arr, &smp))
    {
        return ACQ_ERR_PARAM;
    }

    if (samples == 0u || samples > ACQ_MAX_SAMPLES)
    {
        return ACQ_ERR_PARAM;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    /* Only IDLE or ERROR may start a capture; COMPLETE holds the client's data. */
    if (acq_state == ACQ_STATE_CAPTURING ||
        acq_state == ACQ_STATE_COMPLETE ||
        acq_state == ACQ_STATE_STREAMING)
    {
        __set_PRIMASK(primask);
        return ACQ_ERR_BUSY;
    }

    acq_stop_hw();
    acq_clear_dma_flags();
    ADC1->SR = 0;

    acq_overrun = 0;
    acq_dma_errors = 0;
    acq_captured = 0;
    acq_elapsed_us = 0;
    acq_rate_hz = rate_hz;
    acq_samples = samples;

    /* Sample time and single-channel sequence (channel 0). */
    ADC1->SMPR2 = (ADC1->SMPR2 & ~ADC_SMPR2_SMP0) | (uint32_t)smp;
    ADC1->SQR3  = 0;

    /* Re-arm the DMA for this length (NDTR is writable while the stream is off). */
    DMA2_Stream0->NDTR = (uint16_t)samples;

    /* Timer geometry for this rate. */
    TIM2->CR1 = 0;
    TIM2->PSC = psc;
    TIM2->ARR = arr;
    TIM2->CNT = 0;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR  = 0;

    /* Arm DMA, then start the trigger source last. The ADC is already on. */
    DMA2_Stream0->CR |= DMA_SxCR_EN;

    acq_start_cycles = DWT->CYCCNT;
    acq_start_tick = HAL_GetTick();
    acq_state = ACQ_STATE_CAPTURING;
    acq_capture_id++;

    TIM2->CR1 |= TIM_CR1_CEN;

    __set_PRIMASK(primask);
    return ACQ_OK;
}

ACQ_Status_t ACQ_Reset(void)
{
    if (acq_state == ACQ_STATE_CAPTURING || acq_state == ACQ_STATE_STREAMING)
    {
        return ACQ_ERR_BUSY;
    }

    acq_stop_hw();
    acq_state = ACQ_STATE_IDLE;
    acq_captured = 0;
    acq_elapsed_us = 0;
    acq_overrun = 0;
    acq_dma_errors = 0;
    return ACQ_OK;
}

void ACQ_Task(void)
{
    if (acq_state == ACQ_STATE_CAPTURING)
    {
        uint32_t expected_ms = (acq_samples * 1000u) / acq_rate_hz + 100u;

        if ((HAL_GetTick() - acq_start_tick) > expected_ms)
        {
            acq_dma_errors++;   /* capture never completed: treat as a fault */
            acq_finalize(1);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Status                                                                     */
/* ------------------------------------------------------------------------- */

ACQ_State_t ACQ_GetState(void)     { return acq_state; }
uint32_t    ACQ_GetCaptureId(void) { return acq_capture_id; }
uint32_t    ACQ_GetCapacity(void)  { return ACQ_MAX_SAMPLES; }
uint32_t    ACQ_GetRateHz(void)    { return acq_rate_hz; }
uint32_t    ACQ_GetCaptured(void)  { return acq_captured; }
uint32_t    ACQ_GetElapsedUs(void) { return acq_elapsed_us; }
uint8_t     ACQ_GetOverrun(void)   { return acq_overrun; }
uint8_t     ACQ_GetDmaErrors(void) { return acq_dma_errors; }

uint8_t ACQ_BeginRead(const uint16_t **samples, uint32_t *count)
{
    uint32_t primask;

    if (samples == NULL || count == NULL)
    {
        return 0;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    if (acq_state != ACQ_STATE_COMPLETE)
    {
        __set_PRIMASK(primask);
        return 0;
    }

    acq_state = ACQ_STATE_STREAMING;
    *samples = acq_buffer;
    *count = acq_captured;

    __set_PRIMASK(primask);
    return 1;
}

void ACQ_EndRead(void)
{
    if (acq_state == ACQ_STATE_STREAMING)
    {
        acq_state = ACQ_STATE_COMPLETE;
    }
}
