# STM32 USB Ethernet Webserver

A USB Ethernet device and HTTP webserver built with **STM32F411CEU6 (Black Pill)**, **TinyUSB**, and **lwIP**.

The STM32 exposes a USB CDC-NCM network interface to the host computer. It runs a DHCP server to assign the host an IP address and serves a web page over HTTP.

## Hardware

- STM32F411CEU6 Black Pill
- USB connection to a host computer
- ST-Link V2 for programming and debugging

## Software

- STM32CubeMX-generated HAL project
- TinyUSB USB device stack
- USB CDC-NCM networking
- lwIP TCP/IP stack
- DHCPv4 server
- lwIP HTTP server
- CMake and Ninja build system

## Network configuration

| Device | IP address |
|---|---|
| STM32 | `192.168.7.1` |
| USB host | DHCP-assigned, typically `192.168.7.2` |
| Netmask | `255.255.255.0` |

The webserver is accessible at:

`http://192.168.7.1/`

## Build

Clone the repository and its dependencies:

```bash
git clone --recursive https://github.com/TrafalgarOP/usb-webserver.git
cd usb-webserver
```

Configure and build using the ARM Debug preset:

```bash
cmake --preset Debug
cmake --build --preset Debug
```

## Flash

Example using STM32CubeProgrammer CLI and an ST-Link:

```bash
CLI="$HOME/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI"

"$CLI" -c port=SWD freq=4000 \
  -w build/usb-webserver.elf -v -rst
```

Adjust the CLI path if STM32CubeCLT is installed elsewhere.

## Dependencies

Dependencies are included as Git submodules:

- [TinyUSB](https://github.com/hathach/tinyusb)
- [lwIP](https://github.com/lwip-tcpip/lwip)

To initialize or update them after cloning:

```bash
git submodule update --init --recursive
```

## Status

The USB network interface, DHCP address assignment, ping connectivity, and HTTP GET requests have been tested successfully.