//
// Created by meyti on 11/20/26.
//

#ifndef USB_WEBSERVER_API_SERVER_H
#define USB_WEBSERVER_API_SERVER_H

/*
 * Start the application HTTP API transport on TCP port 8080.
 *
 * Must be called once after lwip_init() and after the netif is up. In this
 * project it is called from API_Init(), which runs from
 * usb_network_services_init() after usb_network_init().
 *
 * The server uses the lwIP raw TCP API with a fixed connection pool. It never
 * allocates memory and never blocks, so it is safe for NO_SYS=1. It handles
 * HTTP framing only; request-line routing lives in api_router.c.
 */
void API_Server_Init(void);

#endif // USB_WEBSERVER_API_SERVER_H