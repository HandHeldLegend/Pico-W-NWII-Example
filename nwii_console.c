/**
 * @file nwii_console.c
 * @brief Line-based USB serial console for driving the example by hand (or from a script):
 *        connect / disconnect, press buttons, move the pointer, swap extensions.
 *
 * Free and unencumbered software released into the public domain (The Unlicense). See LICENSE.
 * Written with the help of Claude Opus (Anthropic).
 *
 * SPDX-License-Identifier: Unlicense
 */

#include "main.h"

#include "nwii_lib.h"
#include "pico/stdlib.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>

#define NWII_CONSOLE_LINE_MAX     96
#define NWII_CONSOLE_PRESS_MS     150 // Default tap length for "press"

typedef enum
{
    NWII_POINTER_CIRCLE = 0, // Slow demo circle, proves IR works with no sensor wired in
    NWII_POINTER_FIXED,      // Held at the position set by "point"
    NWII_POINTER_HIDDEN,     // Remote pointed away from the screen
} nwii_pointer_mode_t;

typedef struct
{
    const char *name;
    uint16_t    offset; // Offset of the bool inside nwii_input_s
} nwii_console_button_s;

#define NWII_BTN(name, field) {name, (uint16_t)offsetof(nwii_input_s, field)}

static const nwii_console_button_s _buttons[] = {
    NWII_BTN("a", remote.a),          NWII_BTN("b", remote.b),
    NWII_BTN("1", remote.one),        NWII_BTN("2", remote.two),
    NWII_BTN("+", remote.plus),       NWII_BTN("-", remote.minus),
    NWII_BTN("plus", remote.plus),    NWII_BTN("minus", remote.minus),
    NWII_BTN("home", remote.home),
    NWII_BTN("up", remote.up),        NWII_BTN("down", remote.down),
    NWII_BTN("left", remote.left),    NWII_BTN("right", remote.right),
    NWII_BTN("c", nunchuk.c),         NWII_BTN("z", nunchuk.z),
    NWII_BTN("cc-a", classic.a),      NWII_BTN("cc-b", classic.b),
    NWII_BTN("cc-x", classic.x),      NWII_BTN("cc-y", classic.y),
    NWII_BTN("cc-l", classic.l),      NWII_BTN("cc-r", classic.r),
    NWII_BTN("cc-zl", classic.zl),    NWII_BTN("cc-zr", classic.zr),
    NWII_BTN("cc-plus", classic.plus), NWII_BTN("cc-minus", classic.minus),
    NWII_BTN("cc-home", classic.home),
    NWII_BTN("cc-up", classic.up),    NWII_BTN("cc-down", classic.down),
    NWII_BTN("cc-left", classic.left), NWII_BTN("cc-right", classic.right),
};

#define NWII_BUTTON_COUNT (sizeof(_buttons) / sizeof(_buttons[0]))

/* Written by the console (main loop), read by the input hook (BTstack context). Each field is a
 * single word, so no locking is needed. */
static volatile bool     _held[NWII_BUTTON_COUNT];
static volatile uint64_t _release_us[NWII_BUTTON_COUNT]; // 0 = no timed release pending
static volatile uint8_t  _pointer_mode = NWII_POINTER_CIRCLE;
static volatile float    _pointer_x = 0.0f;
static volatile float    _pointer_y = 0.0f;
static volatile float    _pointer_roll = 0.0f;
static volatile bool     _stick_set = false;
static volatile uint16_t _stick_x = NWII_STICK_CENTER;
static volatile uint16_t _stick_y = NWII_STICK_CENTER;
static volatile bool     _accel_set = false;
static volatile int16_t  _accel[3] = {0, 0, NWII_ACCEL_1G_MG};
static volatile uint64_t _shake_start_us = 0;
static volatile uint64_t _shake_until_us = 0;

static char    _line[NWII_CONSOLE_LINE_MAX];
static uint8_t _line_len = 0;

static int _nwii_console_find_button(const char *name)
{
    for (unsigned i = 0; i < NWII_BUTTON_COUNT; i++)
    {
        if (strcmp(_buttons[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static void _nwii_console_help(void)
{
    printf("Commands (one per line):\n"
           "  connect | disconnect     radio on and page the Wii | drop the link and stay off (kept across reboots)\n"
           "  cycle                    power-cycle the radio and reconnect (like a dead-link recovery)\n"
           "  forget                   forget the paired Wii and wait for SYNC\n"
           "  status                   connection, extension and input state\n"
           "  press <btn> [ms]         tap a button (default %d ms)\n"
           "  hold <btn> | release <btn|all>\n"
           "  ext none|nunchuk|classic attach or detach an extension\n"
           "  point <x> <y> [roll_deg] hold the pointer (-1..1, +x right, +y up)\n"
           "  circle | still | hide    demo circle | centre | pointed away\n"
           "  stick <x> <y>            nunchuk stick, -1..1 (\"stick off\" recentres)\n"
           "  accel <x> <y> <z>        remote accelerometer in mg (\"accel off\" = flat)\n"
           "  shake [ms]               shake the remote (default 1000 ms)\n"
           "Buttons: a b 1 2 plus minus home up down left right c z\n"
           "         cc-a cc-b cc-x cc-y cc-l cc-r cc-zl cc-zr cc-plus cc-minus cc-home cc-up cc-down cc-left cc-right\n",
           NWII_CONSOLE_PRESS_MS);
}

static void _nwii_console_status(void)
{
    static const char *ext_names[] = {"none", "nunchuk", "classic", "classic pro"};
    static const char *pointer_names[] = {"circle", "fixed", "hidden"};

    const nwii_extension_t ext = nwii_api_get_extension();
    printf("Extension: %s, pointer: %s (%.2f, %.2f, roll %.0f deg)\n",
           (ext < NWII_EXTENSION_MAX) ? ext_names[ext] : "?", pointer_names[_pointer_mode],
           (double)_pointer_x, (double)_pointer_y, (double)(_pointer_roll * 57.29578f));

    printf("Held:");
    bool any = false;
    for (unsigned i = 0; i < NWII_BUTTON_COUNT; i++)
    {
        if (_held[i]) { printf(" %s", _buttons[i].name); any = true; }
    }
    printf(any ? "\n" : " none\n");

    nwii_btc_request(NWII_BTC_REQUEST_STATUS);
}

static bool _nwii_console_parse_float(const char *s, float *out)
{
    if (s == NULL) return false;
    char *end = NULL;
    *out = strtof(s, &end);
    return end != s;
}

static uint16_t _nwii_console_stick(float v)
{
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
    return (uint16_t)(NWII_STICK_CENTER + v * 2047.0f);
}

static void _nwii_console_execute(char *line)
{
    char *argv[5] = {0};
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < 5; tok = strtok(NULL, " \t"))
    {
        for (char *p = tok; *p; p++) *p = (char)tolower((unsigned char)*p);
        argv[argc++] = tok;
    }
    if (argc == 0) return;

    const char *cmd = argv[0];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
    {
        _nwii_console_help();
    }
    else if (!strcmp(cmd, "connect"))
    {
        nwii_btc_request(NWII_BTC_REQUEST_CONNECT);
    }
    else if (!strcmp(cmd, "disconnect"))
    {
        nwii_btc_request(NWII_BTC_REQUEST_DISCONNECT);
    }
    else if (!strcmp(cmd, "cycle"))
    {
        nwii_btc_request(NWII_BTC_REQUEST_CYCLE);
    }
    else if (!strcmp(cmd, "forget") || !strcmp(cmd, "p"))
    {
        nwii_btc_request(NWII_BTC_REQUEST_FORGET);
    }
    else if (!strcmp(cmd, "status"))
    {
        _nwii_console_status();
    }
    else if (!strcmp(cmd, "press") || !strcmp(cmd, "hold") || !strcmp(cmd, "release"))
    {
        if (argc < 2) { printf("Usage: %s <button>\n", cmd); return; }

        if (!strcmp(cmd, "release") && !strcmp(argv[1], "all"))
        {
            for (unsigned i = 0; i < NWII_BUTTON_COUNT; i++) { _held[i] = false; _release_us[i] = 0; }
            printf("Released all\n");
            return;
        }

        const int b = _nwii_console_find_button(argv[1]);
        if (b < 0) { printf("Unknown button '%s' (try help)\n", argv[1]); return; }

        if (!strcmp(cmd, "release"))
        {
            _held[b] = false;
            _release_us[b] = 0;
            printf("Released %s\n", _buttons[b].name);
        }
        else if (!strcmp(cmd, "hold"))
        {
            _release_us[b] = 0;
            _held[b] = true;
            printf("Holding %s\n", _buttons[b].name);
        }
        else
        {
            float ms = NWII_CONSOLE_PRESS_MS;
            if (argc > 2) _nwii_console_parse_float(argv[2], &ms);
            if (ms < 20.0f) ms = 20.0f;
            _release_us[b] = time_us_64() + (uint64_t)(ms * 1000.0f);
            _held[b] = true;
            printf("Pressing %s for %d ms\n", _buttons[b].name, (int)ms);
        }
    }
    else if (!strcmp(cmd, "ext"))
    {
        nwii_extension_t ext;
        if (argc < 2) { printf("Usage: ext none|nunchuk|classic\n"); return; }
        if (!strcmp(argv[1], "none")) ext = NWII_EXTENSION_NONE;
        else if (!strcmp(argv[1], "nunchuk")) ext = NWII_EXTENSION_NUNCHUK;
        else if (!strcmp(argv[1], "classic")) ext = NWII_EXTENSION_CLASSIC_PRO;
        else { printf("Unknown extension '%s'\n", argv[1]); return; }
        nwii_api_set_extension(ext);
        printf("Extension -> %s\n", argv[1]);
    }
    else if (!strcmp(cmd, "point"))
    {
        float x, y, roll_deg = 0.0f;
        if (!_nwii_console_parse_float(argv[1], &x) || !_nwii_console_parse_float(argv[2], &y))
        {
            printf("Usage: point <x> <y> [roll_deg]\n");
            return;
        }
        if (argc > 3) _nwii_console_parse_float(argv[3], &roll_deg);
        _pointer_x = x;
        _pointer_y = y;
        _pointer_roll = roll_deg / 57.29578f;
        _pointer_mode = NWII_POINTER_FIXED;
        printf("Pointer at %.2f, %.2f, roll %.0f deg\n", (double)x, (double)y, (double)roll_deg);
    }
    else if (!strcmp(cmd, "circle"))
    {
        _pointer_mode = NWII_POINTER_CIRCLE;
        printf("Pointer circling\n");
    }
    else if (!strcmp(cmd, "still") || !strcmp(cmd, "s"))
    {
        _pointer_x = 0.0f;
        _pointer_y = 0.0f;
        _pointer_roll = 0.0f;
        _pointer_mode = NWII_POINTER_FIXED;
        printf("Pointer held still at centre\n");
    }
    else if (!strcmp(cmd, "hide"))
    {
        _pointer_mode = NWII_POINTER_HIDDEN;
        printf("Pointer hidden (remote pointed away)\n");
    }
    else if (!strcmp(cmd, "stick"))
    {
        float x, y;
        if (argc > 1 && !strcmp(argv[1], "off"))
        {
            _stick_set = false;
            printf("Stick centred\n");
            return;
        }
        if (!_nwii_console_parse_float(argv[1], &x) || !_nwii_console_parse_float(argv[2], &y))
        {
            printf("Usage: stick <x> <y> | stick off\n");
            return;
        }
        _stick_x = _nwii_console_stick(x);
        _stick_y = _nwii_console_stick(y);
        _stick_set = true;
        printf("Stick at %.2f, %.2f\n", (double)x, (double)y);
    }
    else if (!strcmp(cmd, "accel"))
    {
        float v[3];
        if (argc > 1 && !strcmp(argv[1], "off"))
        {
            _accel_set = false;
            printf("Accelerometer flat\n");
            return;
        }
        if (!_nwii_console_parse_float(argv[1], &v[0]) || !_nwii_console_parse_float(argv[2], &v[1]) ||
            !_nwii_console_parse_float(argv[3], &v[2]))
        {
            printf("Usage: accel <x> <y> <z> (mg) | accel off\n");
            return;
        }
        for (int i = 0; i < 3; i++) _accel[i] = (int16_t)v[i];
        _accel_set = true;
        printf("Accelerometer %d, %d, %d mg\n", _accel[0], _accel[1], _accel[2]);
    }
    else if (!strcmp(cmd, "shake"))
    {
        float ms = 1000.0f;
        if (argc > 1) _nwii_console_parse_float(argv[1], &ms);
        // Whole swings only, so the shake ends at rest
        const uint64_t swings = (uint64_t)(ms / 166.0f) + 1u;
        _shake_start_us = time_us_64();
        _shake_until_us = _shake_start_us + swings * 166000u;
        printf("Shaking for %d ms\n", (int)ms);
    }
    else
    {
        printf("Unknown command '%s' (try help)\n", cmd);
    }
}

void nwii_console_task(void)
{
    for (;;)
    {
        const int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) return;

        if (c == '\r' || c == '\n')
        {
            _line[_line_len] = '\0';
            _line_len = 0;
            printf("> %s\n", _line);
            _nwii_console_execute(_line);
        }
        else if (_line_len < NWII_CONSOLE_LINE_MAX - 1)
        {
            _line[_line_len++] = (char)c;
        }
    }
}

void nwii_console_apply(nwii_input_s *out)
{
    const uint64_t now = time_us_64();

    for (unsigned i = 0; i < NWII_BUTTON_COUNT; i++)
    {
        if (_release_us[i] && now >= _release_us[i])
        {
            _held[i] = false;
            _release_us[i] = 0;
        }
        if (_held[i]) *(bool *)((uint8_t *)out + _buttons[i].offset) = true;
    }

    if (_stick_set)
    {
        out->nunchuk.stick_x = _stick_x;
        out->nunchuk.stick_y = _stick_y;
    }

    if (_accel_set)
    {
        out->accel_x = _accel[0];
        out->accel_y = _accel[1];
        out->accel_z = _accel[2];
    }

    if (now < _shake_until_us)
    {
        // ~6 Hz swing of +/-3 g from the command, as the HOJA core does
        const float phase = (float)((now - _shake_start_us) % 166000u) / 166000.0f;
        const int16_t delta = (int16_t)(3000.0f * sinf(phase * 6.2831853f));
        out->accel_x += delta;
        out->accel_y += delta;
        out->accel_z += delta;
    }

    switch (_pointer_mode)
    {
    case NWII_POINTER_HIDDEN:
        nwii_ir_clear(out->ir);
        break;

    case NWII_POINTER_FIXED:
        nwii_ir_set_pointer_rotated(out->ir, _pointer_x, _pointer_y, _pointer_roll);
        break;

    case NWII_POINTER_CIRCLE:
    default:
    {
        const float t = (float)(now % 6000000u) / 6000000.0f;
        nwii_ir_set_pointer(out->ir, 0.5f * cosf(t * 6.2831853f), 0.5f * sinf(t * 6.2831853f));
        break;
    }
    }
}
