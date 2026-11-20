//
// Created by meyti on 11/18/26.
//

#include <string.h>
#include <stdlib.h>

#include "api.h"
#include "api_server.h"
#include "hardware.h"
#include "httpd.h"
#include "modules/gpio.h"

#if LWIP_HTTPD_CGI

static const char *api_cgi_handler(
    int index,
    int num_params,
    char *params[],
    char *values[])
{
    (void)index;

    const char *device = NULL;
    const char *port_str = NULL;
    const char *pin_str = NULL;
    const char *state_str = NULL;
    const char *mode_str = NULL;
    const char *pull_str = NULL;

    for (int i = 0; i < num_params; i++)
    {
        if (strcmp(params[i], "device") == 0)
            device = values[i];
        else if (strcmp(params[i], "port") == 0)
            port_str = values[i];
        else if (strcmp(params[i], "pin") == 0)
            pin_str = values[i];
        else if (strcmp(params[i], "state") == 0)
            state_str = values[i];
        else if (strcmp(params[i], "mode") == 0)
            mode_str = values[i];
        else if (strcmp(params[i], "pull") == 0)
            pull_str = values[i];
    }

    if (device != NULL && strcmp(device, "gpio") == 0)
    {
        if (port_str != NULL && pin_str != NULL)
        {
            char *end_port;
            char *end_pin;

            long port_value = strtol(port_str, &end_port, 10);
            long pin_value = strtol(pin_str, &end_pin, 10);

            if (*end_port == '\0' && *end_pin == '\0' &&
                port_value >= 0 && port_value <= 1 &&
                pin_value >= 0 && pin_value <= 15)
            {
                uint8_t port = (uint8_t)port_value;
                uint8_t pin = (uint8_t)pin_value;
                GPIO_Status_t status = GPIO_STATUS_OK;

                if (mode_str != NULL)
                {
                    GPIO_Mode_t mode;
                    GPIO_Pull_t pull = GPIO_CONFIG_PULL_NONE;

                    if (strcmp(mode_str, "input") == 0)
                        mode = GPIO_CONFIG_INPUT;
                    else if (strcmp(mode_str, "output") == 0)
                        mode = GPIO_CONFIG_OUTPUT;
                    else
                        return "/index.html";

                    if (pull_str != NULL)
                    {
                        if (strcmp(pull_str, "up") == 0)
                            pull = GPIO_CONFIG_PULL_UP;
                        else if (strcmp(pull_str, "down") == 0)
                            pull = GPIO_CONFIG_PULL_DOWN;
                        else if (strcmp(pull_str, "none") != 0)
                            return "/index.html";
                    }

                    status = GPIO_ConfigurePin(port, pin, mode, pull);
                }

                if (state_str != NULL && status == GPIO_STATUS_OK)
                {
                    if (strcmp(state_str, "0") == 0)
                        status = GPIO_WritePin(port, pin, 0);
                    else if (strcmp(state_str, "1") == 0)
                        status = GPIO_WritePin(port, pin, 1);
                }

                /* Read back pin state for verification and monitoring */
                uint8_t pin_val = 0;
                if (status == GPIO_STATUS_OK)
                {
                    GPIO_ReadPin(port, pin, &pin_val);
                    if (status == GPIO_STATUS_OK)
                    {
                        GPIO_ReadPin(port, pin, &pin_val);
                        // If you have a debug print mechanism or CDC print:
                        // printf("GPIO Port %d Pin %d Read State: %d\n", port, pin, pin_val);
                    }
                    (void)pin_val; /* Available for debugging / future JSON response */
                }
            }
        }
    }

    return "/index.html";
}

static const tCGI cgi_handlers[] =
{
    { "/api", api_cgi_handler }
};

#endif

void API_Init(void)
{
    /* Start the JSON API transport on port 8080. The legacy CGI handler below
     * stays temporarily so the current frontend keeps working; it will be
     * removed once the web UI has migrated to /api/v1. */
    API_Server_Init();

#if LWIP_HTTPD_CGI
    http_set_cgi_handlers(
        cgi_handlers,
        sizeof(cgi_handlers) / sizeof(cgi_handlers[0])
    );
#endif
}