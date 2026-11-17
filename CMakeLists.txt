cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME usb-webserver)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
        Core/Src/usb_network.c
        Core/Src/usb_descriptors.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# TinyUSB
set(TINYUSB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyusb")

include(${TINYUSB_DIR}/src/CMakeLists.txt)

tinyusb_target_add(${CMAKE_PROJECT_NAME})

# Give project headers priority over TinyUSB headers
target_include_directories(${CMAKE_PROJECT_NAME} BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc"
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${TINYUSB_DIR}/src/portable/synopsys/dwc2/dcd_dwc2.c
        ${TINYUSB_DIR}/src/portable/synopsys/dwc2/dwc2_common.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${TINYUSB_DIR}/src/portable/synopsys/dwc2
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        CFG_TUSB_MCU=OPT_MCU_STM32F4
)

# lwIP
set(LWIP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/lwip")

# Host-side lwIP HTML filesystem generator
find_program(HOST_CC gcc REQUIRED)

set(MAKEFSDATA_SOURCE
        "${LWIP_DIR}/src/apps/http/makefsdata/makefsdata.c"
)

set(MAKEFSDATA_EXECUTABLE
        "${CMAKE_CURRENT_BINARY_DIR}/host-tools/makefsdata"
)

add_custom_command(
        OUTPUT "${MAKEFSDATA_EXECUTABLE}"

        COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/host-tools"

        COMMAND "${HOST_CC}"
        "-I${CMAKE_CURRENT_SOURCE_DIR}/tools/makefsdata"
        "-I${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc"
        "-I${LWIP_DIR}/src/include"
        "-I${LWIP_DIR}/src/apps/http"
        "${MAKEFSDATA_SOURCE}"
        -o "${MAKEFSDATA_EXECUTABLE}"

        DEPENDS
        "${MAKEFSDATA_SOURCE}"
        "${LWIP_DIR}/src/apps/http/makefsdata/tinydir.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/makefsdata/lwipopts.h"

        VERBATIM
)

# Website source files
file(GLOB_RECURSE WEB_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/web/*"
)

set(GENERATED_FSDATA
        "${CMAKE_CURRENT_SOURCE_DIR}/fsdata_custom.c"
)

add_custom_command(
        OUTPUT "${GENERATED_FSDATA}"

        COMMAND "${MAKEFSDATA_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/web"
        "-f:${GENERATED_FSDATA}"

        DEPENDS
        "${MAKEFSDATA_EXECUTABLE}"
        ${WEB_FILES}

        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM
)

add_custom_target(generate_web_fs
        DEPENDS "${GENERATED_FSDATA}"
)

add_dependencies(${CMAKE_PROJECT_NAME} generate_web_fs)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LWIP_DIR}/src/include"
        "${LWIP_DIR}/src/include/ipv4"
        "${LWIP_DIR}/src/include/lwip/apps"
        "${TINYUSB_DIR}/lib/networking"
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        # lwIP core
        "${LWIP_DIR}/src/core/altcp.c"
        "${LWIP_DIR}/src/core/altcp_alloc.c"
        "${LWIP_DIR}/src/core/altcp_tcp.c"
        "${LWIP_DIR}/src/core/def.c"
        "${LWIP_DIR}/src/core/dns.c"
        "${LWIP_DIR}/src/core/inet_chksum.c"
        "${LWIP_DIR}/src/core/init.c"
        "${LWIP_DIR}/src/core/ip.c"
        "${LWIP_DIR}/src/core/mem.c"
        "${LWIP_DIR}/src/core/memp.c"
        "${LWIP_DIR}/src/core/netif.c"
        "${LWIP_DIR}/src/core/pbuf.c"
        "${LWIP_DIR}/src/core/raw.c"
        "${LWIP_DIR}/src/core/stats.c"
        "${LWIP_DIR}/src/core/sys.c"
        "${LWIP_DIR}/src/core/tcp.c"
        "${LWIP_DIR}/src/core/tcp_in.c"
        "${LWIP_DIR}/src/core/tcp_out.c"
        "${LWIP_DIR}/src/core/timeouts.c"
        "${LWIP_DIR}/src/core/udp.c"

        # IPv4
        "${LWIP_DIR}/src/core/ipv4/autoip.c"
        "${LWIP_DIR}/src/core/ipv4/dhcp.c"
        "${LWIP_DIR}/src/core/ipv4/etharp.c"
        "${LWIP_DIR}/src/core/ipv4/icmp.c"
        "${LWIP_DIR}/src/core/ipv4/igmp.c"
        "${LWIP_DIR}/src/core/ipv4/ip4.c"
        "${LWIP_DIR}/src/core/ipv4/ip4_addr.c"
        "${LWIP_DIR}/src/core/ipv4/ip4_frag.c"

        # Ethernet interface and HTTP server
        "${LWIP_DIR}/src/netif/ethernet.c"
        "${LWIP_DIR}/src/apps/http/httpd.c"
        "${LWIP_DIR}/src/apps/http/fs.c"

        # DHCP server
        "Core/Src/dhserver.c"
)
