//
// Created by meyti on 11/18/26.
//

#ifndef USB_WEBSERVER_HARDWARE_H
#define USB_WEBSERVER_HARDWARE_H

#include <stdint.h>


void Hardware_Init(void);
void Hardware_Task(void);

void Hardware_GPIO_SetLED(uint8_t state);
#endif //USB_WEBSERVER_HARDWARE_H
