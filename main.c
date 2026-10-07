/**
 * @file main.c
 * @brief Example application entry point and NWII-LIB-HID callback implementations.
 *
 * Free and unencumbered software released into the public domain (The Unlicense). See LICENSE.
 * Written with the help of Claude Opus (Anthropic).
 *
 * SPDX-License-Identifier: Unlicense
 */

#include "main.h"

#include "nwii_lib.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include <math.h>

/* Boot strap: hold GP1 low at power-up to forget the saved Wii and wait for SYNC. */
static const uint NWII_PAIR_BOOT_PIN = 1;

/* Hold GP0 low to pin the pointer to the centre instead of the demo circle. */
static const uint NWII_POINTER_HOLD_PIN = 0;

/* Buttons are active-low with pull-ups. */
static const uint NWII_A_BUTTON_PIN     = 14;
static const uint NWII_B_BUTTON_PIN     = 15;
static const uint NWII_ONE_BUTTON_PIN   = 16;
static const uint NWII_TWO_BUTTON_PIN   = 17;
static const uint NWII_PLUS_BUTTON_PIN  = 18;
static const uint NWII_MINUS_BUTTON_PIN = 19;
static const uint NWII_HOME_BUTTON_PIN  = 20;

/* Tap GP21 low to cycle the extension: none -> Nunchuk -> Classic Controller Pro. */
static const uint NWII_EXTENSION_PIN    = 21;

/* Example controller address used for Bluetooth bring-up. */
const uint8_t device_mac[6] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF7};
nwii_storage_s device_storage = {0};
volatile bool nwii_pointer_still = false;

static void _nwii_input_pin_init(uint pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

static inline bool _nwii_pressed(uint pin)
{
    return !gpio_get(pin);
}

int main()
{
    _nwii_input_pin_init(NWII_PAIR_BOOT_PIN);
    _nwii_input_pin_init(NWII_POINTER_HOLD_PIN);
    _nwii_input_pin_init(NWII_A_BUTTON_PIN);
    _nwii_input_pin_init(NWII_B_BUTTON_PIN);
    _nwii_input_pin_init(NWII_ONE_BUTTON_PIN);
    _nwii_input_pin_init(NWII_TWO_BUTTON_PIN);
    _nwii_input_pin_init(NWII_PLUS_BUTTON_PIN);
    _nwii_input_pin_init(NWII_MINUS_BUTTON_PIN);
    _nwii_input_pin_init(NWII_HOME_BUTTON_PIN);
    _nwii_input_pin_init(NWII_EXTENSION_PIN);

    sleep_ms(10);
    bool pairing_mode = _nwii_pressed(NWII_PAIR_BOOT_PIN);

    stdio_init_all();

    /* Give a serial monitor a few seconds to attach so the boot log is not lost. */
    for (int i = 0; i < 300 && !stdio_usb_connected(); i++)
    {
        sleep_ms(10);
    }
    sleep_ms(250);
    printf("\nPico-W-NWII-Example booted\n");

    nwii_flash_init();

    /* Load the saved Wii address, if one was written previously. */
    nwii_flash_read((uint8_t *)&device_storage, NWII_STORAGE_SIZE, NWII_STORAGE_PAGE);

    if (device_storage.magic_byte != NWII_STORAGE_MAGIC)
    {
        memset(&device_storage, 0, NWII_STORAGE_SIZE);
        device_storage.magic_byte = NWII_STORAGE_MAGIC;
    }

    printf("Saved Wii: %02X:%02X:%02X:%02X:%02X:%02X\n",
           device_storage.host_mac[0], device_storage.host_mac[1], device_storage.host_mac[2],
           device_storage.host_mac[3], device_storage.host_mac[4], device_storage.host_mac[5]);

    nwii_device_config_s config = {
        .extension = NWII_EXTENSION_NONE,
    };

    if (nwii_api_init(&config))
    {
        printf("Entering Bluetooth mode (pairing %s)\n", pairing_mode ? "on" : "off");
        nwii_btc_enter(device_mac, pairing_mode);
    }

    printf("nwii_api_init failed\n");
    for (;;) tight_loop_contents();
}

/* -------------------------------------------------------------------------- */
/* NWII-LIB-HID callback implementations                                       */
/* -------------------------------------------------------------------------- */

/* Cycles the extension on each GP21 tap so hotplug can be tested without a full controller. */
static void _nwii_extension_button_task(void)
{
    static bool was_pressed = false;
    const bool pressed = _nwii_pressed(NWII_EXTENSION_PIN);

    if (pressed && !was_pressed)
    {
        nwii_extension_t next = (nwii_extension_t)(nwii_api_get_extension() + 1);
        if (next == NWII_EXTENSION_CLASSIC) next = NWII_EXTENSION_CLASSIC_PRO;
        if (next >= NWII_EXTENSION_MAX) next = NWII_EXTENSION_NONE;

        printf("Extension -> %d\n", (int)next);
        nwii_api_set_extension(next);
    }

    was_pressed = pressed;
}

void nwii_api_hook_get_input(nwii_input_s *out)
{
    /*
     * Keep this path light: the library calls it for every input report, so production firmware
     * should copy from already-sampled input state.
     */
    _nwii_extension_button_task();

    out->remote.a     = _nwii_pressed(NWII_A_BUTTON_PIN);
    out->remote.b     = _nwii_pressed(NWII_B_BUTTON_PIN);
    out->remote.one   = _nwii_pressed(NWII_ONE_BUTTON_PIN);
    out->remote.two   = _nwii_pressed(NWII_TWO_BUTTON_PIN);
    out->remote.plus  = _nwii_pressed(NWII_PLUS_BUTTON_PIN);
    out->remote.minus = _nwii_pressed(NWII_MINUS_BUTTON_PIN);
    out->remote.home  = _nwii_pressed(NWII_HOME_BUTTON_PIN);

    /* With no sensor wired in, trace a slow circle so the Wii Menu cursor proves IR works. */
    float x = 0.0f;
    float y = 0.0f;
    if (!_nwii_pressed(NWII_POINTER_HOLD_PIN) && !nwii_pointer_still)
    {
        const float t = (float)(time_us_64() % 6000000u) / 6000000.0f;
        x = 0.5f * cosf(t * 6.2831853f);
        y = 0.5f * sinf(t * 6.2831853f);
    }
    nwii_ir_set_pointer(out->ir, x, y);
}

void nwii_api_hook_set_rumble(bool enable)
{
    /* No motor on a bare Pico W; use the log to confirm the host's requests. */
    printf("Rumble %s\n", enable ? "on" : "off");
}

void nwii_api_hook_set_leds(uint8_t led_mask)
{
    printf("Player LEDs: %c%c%c%c\n",
           (led_mask & 1) ? '*' : '-', (led_mask & 2) ? '*' : '-',
           (led_mask & 4) ? '*' : '-', (led_mask & 8) ? '*' : '-');
}

void nwii_api_hook_get_power(nwii_power_s *out)
{
    /* Report a full battery to keep the example deterministic. */
    out->level = 0xFF;
    out->low = false;
}
