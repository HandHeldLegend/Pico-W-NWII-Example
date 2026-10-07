# Pico-W-NWII-Example build configuration
#
# Free and unencumbered software released into the public domain (The Unlicense). See LICENSE.
# Written with the help of Claude Opus (Anthropic).
#
# SPDX-License-Identifier: Unlicense

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(Pico-W-NWII-Example C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(Pico-W-NWII-Example
    main.c
    nwii_btc.c
    nwii_console.c
    nwii_flash.c
)

pico_set_program_name(Pico-W-NWII-Example "Pico-W-NWII-Example")
pico_set_program_version(Pico-W-NWII-Example "0.1")

# Set NWII_EXAMPLE_HCI_DUMP to 1 to print every HCI packet (very verbose, but shows exactly how
# far the Wii gets through discovery, pairing and the L2CAP handshake).
target_compile_definitions(Pico-W-NWII-Example PRIVATE
    NWII_EXAMPLE_HCI_DUMP=1
)

target_link_libraries(Pico-W-NWII-Example
        pico_stdlib
        pico_rand
)

# Local includes for the example sources (and btstack_config.h).
target_include_directories(Pico-W-NWII-Example PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Import NWII-LIB-HID library
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/external/NWII-LIB-HID")

target_link_libraries(Pico-W-NWII-Example
    nwii_lib_hid
)

# Pico W radio stack: BTstack Classic over the CYW43.
target_link_libraries(Pico-W-NWII-Example
    pico_cyw43_arch_none
    pico_btstack_cyw43
    pico_btstack_hci_transport_cyw43
    pico_btstack_flash_bank
    pico_btstack_ble
    pico_btstack_classic
)

pico_add_extra_outputs(Pico-W-NWII-Example)

# Logs go out over USB CDC, so no UART adapter is needed. This also exposes picotool's reset
# interface, so `picotool load -f` can reflash a running board without touching BOOTSEL.
pico_enable_stdio_uart(Pico-W-NWII-Example 0)
pico_enable_stdio_usb(Pico-W-NWII-Example 1)
