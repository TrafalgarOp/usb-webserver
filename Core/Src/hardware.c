//
// Created by meyti on 11/18/26.
//

#include "hardware.h"
#include "hardware_config.h"

#if ENABLE_GPIO
#include "modules/gpio.h"
#endif

void Hardware_Init(void)
{
#if ENABLE_GPIO
    GPIO_Init();
#endif
}

void Hardware_Task(void)
{
#if ENABLE_GPIO
    GPIO_Task();
#endif
}

void Hardware_GPIO_SetLED(uint8_t state)
{
#if ENABLE_GPIO
    GPIO_SetLED(state);
#else
    (void)state;
#endif
}