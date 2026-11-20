//
// Created by meyti on 11/18/26.
//

#ifndef USB_WEBSERVER_GPIO_H
#define USB_WEBSERVER_GPIO_H

#include <stdint.h>

typedef enum
{
    GPIO_CONFIG_INPUT = 0,
    GPIO_CONFIG_OUTPUT
} GPIO_Mode_t;

typedef enum
{
    GPIO_CONFIG_PULL_NONE = 0,
    GPIO_CONFIG_PULL_UP,
    GPIO_CONFIG_PULL_DOWN
} GPIO_Pull_t;

typedef enum
{
    GPIO_STATUS_OK = 0,
    GPIO_STATUS_INVALID_PIN,
    GPIO_STATUS_INVALID_MODE,
    GPIO_STATUS_ERROR
} GPIO_Status_t;

/* Port indices used by the API to address pins. */
#define GPIO_PORT_A     0
#define GPIO_PORT_B     1
#define GPIO_PORT_COUNT 2

/*
 * Read-only metadata for a single pin, owned by the GPIO module.
 *
 * mode, pull, and value are only meaningful when configured == 1. For an
 * unconfigured pin they are zeroed defensively and must not be interpreted:
 * a zeroed entry is not "an input with no pull and a low level".
 */
typedef struct
{
    uint8_t     configured; /* 1 once the pin has been configured */
    GPIO_Mode_t mode;       /* valid when configured */
    GPIO_Pull_t pull;       /* valid when configured */
    uint8_t     value;      /* valid when configured: sampled pad level (IDR),
                             * not necessarily the output latch (ODR). Under
                             * electrical loading or contention a driven output
                             * can read back differently from the value set. */
} GPIO_PinInfo_t;

void GPIO_Init(void);
void GPIO_Task(void);

GPIO_Status_t GPIO_ConfigurePin(uint8_t port, uint8_t pin,
                                GPIO_Mode_t mode, GPIO_Pull_t pull);

GPIO_Status_t GPIO_WritePin(uint8_t port, uint8_t pin, uint8_t state);
GPIO_Status_t GPIO_ReadPin(uint8_t port, uint8_t pin, uint8_t *state);

/*
 * Firmware whitelist. Returns 1 only for pins the API may address. This is the
 * single authority for "is this an acceptable pin"; callers must not
 * re-implement the rule.
 */
uint8_t GPIO_IsPinAllowed(uint8_t port, uint8_t pin);

/*
 * Fill *info with read-only metadata for (port, pin).
 *   GPIO_STATUS_OK           - inside the whitelist
 *   GPIO_STATUS_INVALID_PIN  - outside the whitelist
 *   GPIO_STATUS_ERROR        - info is NULL
 * Allowed but never-configured pins report configured == 0 and leave
 * mode/pull/value zero; see GPIO_PinInfo_t for why that must not be read as a
 * real configuration.
 */
GPIO_Status_t GPIO_GetPinInfo(uint8_t port, uint8_t pin, GPIO_PinInfo_t *info);

void GPIO_SetLED(uint8_t state);

#endif //USB_WEBSERVER_GPIO_H
