//
// Created by meyti on 11/21/26.
//
// Optional SWO/ITM console back-end for printf.
//
// Core/Src/syscalls.c routes _write() through a weak, undefined __io_putchar(),
// which the compiler otherwise lowers to a no-op. Providing a strong definition
// here gives printf a real sink. It is deliberately opt-in at configure time
// (USB_WS_ENABLE_ITM_LOG): the default build keeps a harmless stub so no build
// can block on an unattached or non-listening debugger.
//
// ITM/SWO availability still depends on the board routing its SWO pin to the
// probe; enabling the option only makes the firmware capable of emitting trace.
//

#include "stm32f4xx.h"

/*
 * printf() byte sink.
 *
 * With logging enabled, write to ITM stimulus port 0, but only when the ITM and
 * that port are actually enabled. When logging is disabled, do nothing at all.
 * Either way the byte is reported as written so printf never retries or fails.
 */
int __io_putchar(int ch)
{
#if defined(USB_WS_ENABLE_ITM_LOG)
    if ((ITM->TCR & ITM_TCR_ITMENA_Msk) != 0u && (ITM->TER & 1u) != 0u)
    {
        // Bounded wait on the FIFO: a debugger that stops draining SWO must be
        // able to cost a dropped byte, never a stalled superloop.
        uint32_t spin = 2000u;

        while (ITM->PORT[0].u32 == 0u && spin-- != 0u)
        {
        }

        if (spin != 0u)
        {
            ITM->PORT[0].u8 = (uint8_t)ch;
        }
    }
#else
    (void)ch;
#endif

    return ch;
}
