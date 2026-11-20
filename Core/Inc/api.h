//
// Created by meyti on 11/18/26.
//

#ifndef USB_WEBSERVER_API_H
#define USB_WEBSERVER_API_H

/*
 * Firmware version reported by GET /api/v1/system.
 *
 * Compile-time constant. Override with -DAPI_FIRMWARE_VERSION=\"x.y.z\" if the
 * build system starts injecting it later.
 */
#ifndef API_FIRMWARE_VERSION
#define API_FIRMWARE_VERSION "0.1.0"
#endif

void API_Init(void);

#endif //USB_WEBSERVER_API_H