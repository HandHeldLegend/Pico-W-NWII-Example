/**
 * @file main.h
 * @brief Shared declarations for the Pico W Wii Remote example.
 *
 * Free and unencumbered software released into the public domain (The Unlicense). See LICENSE.
 * Written with the help of Claude Opus (Anthropic).
 *
 * SPDX-License-Identifier: Unlicense
 */

#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "nwii_lib_types.h"

typedef struct
{
    uint32_t magic_byte;
    /* Address of the Wii we paired with, so later boots can reconnect to it. */
    uint8_t host_mac[6];
    /* NWII_STORAGE_OFFLINE after a console "disconnect": boot with the radio off. Any other value
     * (including older saves) means connect as usual. */
    uint8_t offline;
} nwii_storage_s;

/* Simple single-page settings block used by the example's flash helper. */
#define NWII_STORAGE_MAGIC 0x57494931 // "WII1"
#define NWII_STORAGE_SIZE sizeof(nwii_storage_s)
#define NWII_STORAGE_PAGE 0
#define NWII_STORAGE_OFFLINE 0xA5

/* Device identity and pairing storage owned by main.c. */
extern const uint8_t device_mac[6];
extern nwii_storage_s device_storage;

/* Flash persistence helpers. */
bool nwii_flash_write(uint8_t *data, uint32_t size, uint32_t page);
bool nwii_flash_read(uint8_t *out, uint32_t size, uint32_t page);
void nwii_flash_task();
void nwii_flash_init();

/* Bluetooth transport entry point. */
void nwii_btc_enter(const uint8_t device_mac[6], bool pairing_mode);

/* Console requests, carried out on the BTstack thread. */
typedef enum
{
    NWII_BTC_REQUEST_CONNECT    = 1u << 0, // Radio on, page the saved Wii, stay online
    NWII_BTC_REQUEST_DISCONNECT = 1u << 1, // Drop the link, radio off, stay offline
    NWII_BTC_REQUEST_CYCLE      = 1u << 2, // Power-cycle the radio and reconnect
    NWII_BTC_REQUEST_FORGET     = 1u << 3, // Forget the Wii and wait for SYNC
    NWII_BTC_REQUEST_STATUS     = 1u << 4, // Print the connection state
} nwii_btc_request_t;

void nwii_btc_request(nwii_btc_request_t request);

/* USB serial console (see nwii_console.c; send "help"). */
void nwii_console_task(void);
void nwii_console_apply(nwii_input_s *out);

#endif
