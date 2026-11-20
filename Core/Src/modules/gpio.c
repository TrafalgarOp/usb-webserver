//
// Created by meyti on 11/18/26.
//


#include <string.h>

#include "main.h"
#include "modules/gpio.h"

#define GPIO_PIN_BIT(pin) (1U << (pin))

/* Track configuration for GPIOA and GPIOB. */
static uint8_t pin_configured[GPIO_PORT_COUNT][16];
static GPIO_Mode_t pin_mode[GPIO_PORT_COUNT][16];
static GPIO_Pull_t pin_pull[GPIO_PORT_COUNT][16];

/* Return the HAL GPIO port. */
static GPIO_TypeDef *GPIO_GetPort(uint8_t port)
{
    switch (port)
    {
        case GPIO_PORT_A:
            return GPIOA;

        case GPIO_PORT_B:
            return GPIOB;

        default:
            return NULL;
    }
}

/* Firmware-level GPIO whitelist. */
uint8_t GPIO_IsPinAllowed(uint8_t port, uint8_t pin)
{
    if (pin > 15)
        return 0;

    if (port == GPIO_PORT_A)
        return (pin >= 1 && pin <= 8) || pin == 15;

    if (port == GPIO_PORT_B)
        return pin <= 10 || (pin >= 12 && pin <= 15);

    return 0;
}

/* Initialize the GPIO module. */
void GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    for (uint8_t port = 0; port < GPIO_PORT_COUNT; port++)
    {
        for (uint8_t pin = 0; pin < 16; pin++)
        {
            pin_configured[port][pin] = 0;
            pin_mode[port][pin] = GPIO_CONFIG_INPUT;
            pin_pull[port][pin] = GPIO_CONFIG_PULL_NONE;
        }
    }

    /*
     * PB0 is already configured as output by CubeMX.
     * Preserve its existing LED functionality.
     */
    pin_configured[GPIO_PORT_B][0] = 1;
    pin_mode[GPIO_PORT_B][0] = GPIO_CONFIG_OUTPUT;
    pin_pull[GPIO_PORT_B][0] = GPIO_CONFIG_PULL_NONE;
}

/* Periodic GPIO task. */
void GPIO_Task(void)
{
    /* Reserved for future GPIO monitoring. */
}

/* Configure a GPIO pin. */
GPIO_Status_t GPIO_ConfigurePin(
    uint8_t port,
    uint8_t pin,
    GPIO_Mode_t mode,
    GPIO_Pull_t pull)
{
    if (!GPIO_IsPinAllowed(port, pin))
        return GPIO_STATUS_INVALID_PIN;

    if (mode != GPIO_CONFIG_INPUT &&
        mode != GPIO_CONFIG_OUTPUT)
        return GPIO_STATUS_INVALID_MODE;

    if (pull != GPIO_CONFIG_PULL_NONE &&
        pull != GPIO_CONFIG_PULL_UP &&
        pull != GPIO_CONFIG_PULL_DOWN)
        return GPIO_STATUS_ERROR;

    GPIO_InitTypeDef config = {0};

    config.Pin = GPIO_PIN_BIT(pin);

    config.Pull = pull == GPIO_CONFIG_PULL_UP ? GPIO_PULLUP :
                  pull == GPIO_CONFIG_PULL_DOWN ? GPIO_PULLDOWN :
                  GPIO_NOPULL;

    config.Speed = GPIO_SPEED_FREQ_LOW;

    config.Mode = mode == GPIO_CONFIG_INPUT ?
                  GPIO_MODE_INPUT :
                  GPIO_MODE_OUTPUT_PP;

    HAL_GPIO_Init(GPIO_GetPort(port), &config);

    pin_mode[port][pin] = mode;
    pin_pull[port][pin] = pull;
    pin_configured[port][pin] = 1;

    return GPIO_STATUS_OK;
}

/* Write to a configured output pin. */
GPIO_Status_t GPIO_WritePin(
    uint8_t port,
    uint8_t pin,
    uint8_t state)
{
    if (!GPIO_IsPinAllowed(port, pin))
        return GPIO_STATUS_INVALID_PIN;

    if (!pin_configured[port][pin] ||
        pin_mode[port][pin] != GPIO_CONFIG_OUTPUT)
        return GPIO_STATUS_INVALID_MODE;

    HAL_GPIO_WritePin(
        GPIO_GetPort(port),
        GPIO_PIN_BIT(pin),
        state ? GPIO_PIN_SET : GPIO_PIN_RESET
    );

    return GPIO_STATUS_OK;
}

/* Read a configured GPIO pin. */
GPIO_Status_t GPIO_ReadPin(
    uint8_t port,
    uint8_t pin,
    uint8_t *state)
{
    if (state == NULL)
        return GPIO_STATUS_ERROR;

    if (!GPIO_IsPinAllowed(port, pin))
        return GPIO_STATUS_INVALID_PIN;

    if (!pin_configured[port][pin])
        return GPIO_STATUS_INVALID_MODE;

    *state = HAL_GPIO_ReadPin(GPIO_GetPort(port), GPIO_PIN_BIT(pin))
    == GPIO_PIN_SET ? 1 : 0;

    return GPIO_STATUS_OK;
}

/* Read-only metadata for a single pin. */
GPIO_Status_t GPIO_GetPinInfo(
    uint8_t port,
    uint8_t pin,
    GPIO_PinInfo_t *info)
{
    if (info == NULL)
        return GPIO_STATUS_ERROR;

    memset(info, 0, sizeof *info);

    if (!GPIO_IsPinAllowed(port, pin))
        return GPIO_STATUS_INVALID_PIN;

    if (pin_configured[port][pin])
    {
        info->configured = 1;
        info->mode = pin_mode[port][pin];
        info->pull = pin_pull[port][pin];
        info->value = HAL_GPIO_ReadPin(GPIO_GetPort(port),
                                       GPIO_PIN_BIT(pin)) == GPIO_PIN_SET
                          ? 1 : 0;
    }

    return GPIO_STATUS_OK;
}

/* Control the existing PB0 LED. */
void GPIO_SetLED(uint8_t state)
{
    HAL_GPIO_WritePin(
        GPIOB,
        GPIO_PIN_0,
        state ? GPIO_PIN_SET : GPIO_PIN_RESET
    );
}
