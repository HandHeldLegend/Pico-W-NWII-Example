/**
 * @file nwii_btc.c
 * @brief Bluetooth Classic HID transport for the Pico W Wii Remote example.
 *
 * Free and unencumbered software released into the public domain (The Unlicense). See LICENSE.
 * Written with the help of Claude Opus (Anthropic).
 *
 * SPDX-License-Identifier: Unlicense
 */

#include "main.h"

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/btstack_chipset_cyw43.h"
#include "btstack.h"

#include "hci_dump.h"
#if NWII_EXAMPLE_HCI_DUMP
#include "hci_dump_embedded_stdout.h"
#endif

#include <stdarg.h>

#include "nwii_lib.h"

/* A real remote reports at ~100 Hz. */
static const uint32_t _btc_poll_rate_ms = 10;
static uint32_t _btc_last_hid_report_timestamp_ms = 0;
static btstack_timer_source_t hid_timer;

/* A paired remote connects to the Wii, never the reverse, so keep paging until it answers. */
static const uint32_t _btc_reconnect_ms = 1000;

/* Quitting or launching a title can leave the Wii holding the link without taking our reports,
 * with nothing on the link to say so. If no report has gone out for this long, power-cycle the
 * radio and reconnect. With several remotes connected the Wii can take a second or two to service
 * a link, so this stays well above that. */
static const uint32_t _btc_stall_ms = 6000;
static btstack_timer_source_t stall_watchdog_timer;
static uint32_t _btc_last_can_send_ms = 0;
static bool _btc_offline = false; /* Console "disconnect": radio stays off until "connect" */
static bool _btc_hid_open = false; /* hid_cid is assigned before the connection finishes opening */
static hci_con_handle_t wii_acl_handle = HCI_CON_HANDLE_INVALID;

/* Powering off, the Wii only closes the HID channels (as when quitting a title), so reconnecting
 * starts as usual. A Wii in standby still accepts a new link but never answers the HID channel
 * request (L2CAP RTX timeout), while a reloading Wii refuses it outright until it is ready. On the
 * standby answer, stop like a real remote does instead of paging again. */
static bool _btc_reconnecting = false; /* Paging again after the Wii closed an open connection */
static bool _btc_fresh_acl = false;    /* This attempt brought up a new link */

/* Faster still: a Wii going to standby takes the page at the radio (it switches roles) and then
 * never completes the connection, which otherwise ends only on a ~20 s timeout. While the Wii
 * reloads for a title, a page has completed within ~3.5 s of the role switch; leaving the Homebrew
 * Channel can take longer, so allow 10 s. */
static const uint32_t _btc_page_stall_ms = 10000;
static btstack_timer_source_t page_stall_timer;
static bool _btc_page_pending = false;  /* Reconnect page in progress */
static bool _btc_page_answered = false; /* ...and the Wii's radio has taken it */

static void _nwii_btc_wii_standby(const char *why);

/* Page timeout while reconnecting (x 0.625 ms, ~5 s) so retries during a reload stay short */
#define NWII_BTC_PAGE_TIMEOUT 0x2000
static btstack_timer_source_t reconnect_timer;

static bool hid_device_pair_enabled = false;

/* The Wii's SYNC search only answers devices listening on the Limited Inquiry Access Code. */
static bool _btc_iac_pending = true;

/* Scratch buffers for SDP records published to the host. */
static uint8_t hid_service_buffer[NWII_HID_SDP_RECORD_LEN] = {0};
static uint8_t pnp_service_buffer[100] = {0};
static btstack_packet_callback_registration_t hci_event_callback_registration;
static uint16_t hid_cid = 0;

static void _nwii_btc_print_hex(const char *tag, const uint8_t *data, uint16_t len)
{
    printf("%s", tag);
    for (uint16_t i = 0; i < len; i++) printf(" %02X", data[i]);
    printf("\n");
}

static bool _nwii_btc_host_saved(void)
{
    for (int i = 0; i < 6; i++)
    {
        if (device_storage.host_mac[i] != 0x00 && device_storage.host_mac[i] != 0xFF) return true;
    }
    return false;
}

static void _page_stall_timer_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    if (_btc_page_pending && _btc_page_answered && _btc_reconnecting)
        _nwii_btc_wii_standby("Wii took the page but never finished connecting: going to standby");
    _btc_page_pending = false;
}

static void _nwii_btc_connect_saved_host(void)
{
    _btc_fresh_acl = false;
    if (_btc_reconnecting)
    {
        _btc_page_pending = true;
        _btc_page_answered = false;
        btstack_run_loop_remove_timer(&page_stall_timer);
        btstack_run_loop_set_timer_handler(&page_stall_timer, &_page_stall_timer_handler);
        btstack_run_loop_set_timer(&page_stall_timer, _btc_page_stall_ms);
        btstack_run_loop_add_timer(&page_stall_timer);
    }
    printf("Paging saved Wii %s\n", bd_addr_to_str(device_storage.host_mac));
    uint8_t status = hid_device_connect(device_storage.host_mac, &hid_cid);
    if (status) printf("hid_device_connect error 0x%02X\n", status);
}

static void _reconnect_timer_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    if (!hid_cid && !_btc_offline) _nwii_btc_connect_saved_host();
}

/* Re-arm packet transmission after a short delay when the poll interval has not elapsed yet. */
static void _hid_timer_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    if (hid_cid) hid_device_request_can_send_now_event(hid_cid);
}

static inline void _nwii_btc_hid_tunnel(const uint8_t *report, uint16_t len)
{
    uint8_t new_report[NWII_INPUT_REPORT_MAX + 1] = {0};
    /* Bluetooth HID input traffic is wrapped with the 0xA1 data prefix before the report body. */
    new_report[0] = 0xA1;
    memcpy(&new_report[1], report, len);

    if (hid_cid)
    {
        hid_device_send_interrupt_message(hid_cid, new_report, len + 1);
    }
}

/* Output reports from the Wii arrive as payload only; rebuild the report id at byte 0. */
static void _nwii_btc_outputreport_handler(uint16_t cid, hid_report_type_t report_type, uint16_t report_id,
                                           int report_size, uint8_t *report)
{
    if (cid != hid_cid) return;
    if (!report || report_size <= 0) return;

    uint8_t data[NWII_OUTPUT_REPORT_MAX] = {0};
    data[0] = (uint8_t)report_id;
    if (report_size > NWII_OUTPUT_REPORT_MAX - 1) report_size = NWII_OUTPUT_REPORT_MAX - 1;
    memcpy(&data[1], report, (size_t)report_size);

    // Speaker data streams continuously; keep it out of the log
    if (report_id != 0x18)
    {
        printf("[type %d] ", (int)report_type);
        _nwii_btc_print_hex("OUT", data, (uint16_t)(report_size + 1));
    }

    nwii_api_output_tunnel(data, (uint16_t)(report_size + 1));
}

/* Some hosts deliver output reports as SET_REPORT on the control channel instead. */
static void _nwii_btc_setreport_handler(uint16_t cid, hid_report_type_t report_type, int report_size, uint8_t *report)
{
    if (cid != hid_cid) return;
    if (!report || report_size <= 0) return;

    printf("[set_report type %d] ", (int)report_type);
    _nwii_btc_print_hex("OUT", report, (uint16_t)report_size);
    nwii_api_output_tunnel(report, (uint16_t)report_size);
}

/* Listen on both the limited (Wii SYNC) and general inquiry access codes. Sent as soon as the
 * controller can take a command, since BTstack has no GAP call for two IACs. */
static void _nwii_btc_write_iac_task(void)
{
    if (!_btc_iac_pending || !hci_can_send_command_packet_now()) return;

    _btc_iac_pending = false;
    hci_send_cmd(&hci_write_current_iac_lap_two_iacs, 2, NWII_HID_INQUIRY_ACCESS_CODE, GAP_IAC_GENERAL_INQUIRY);
    printf("Inquiry access codes: limited + general\n");
}

/*
 * When a title starts, the Wii reloads its system software but keeps the ACL link, and its new
 * stack addresses L2CAP channels that no longer exist. BTstack answers with a Command Reject
 * ("invalid CID") and otherwise drops the traffic, leaving the remote unregistered until the link
 * times out. BTstack's HCI packet-log hook is the only place that reject is visible, so watch for
 * it there, drop the stale link and reconnect straight away, as a real remote does.
 */
static btstack_timer_source_t stale_link_timer;
static hci_con_handle_t stale_link_handle = HCI_CON_HANDLE_INVALID;

static void _stale_link_timer_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    if (stale_link_handle != HCI_CON_HANDLE_INVALID)
    {
        printf("Wii reloaded with stale L2CAP channels; reconnecting\n");
        gap_disconnect(stale_link_handle);
        stale_link_handle = HCI_CON_HANDLE_INVALID;
    }
}

static void _stale_link_watch_log_packet(uint8_t packet_type, uint8_t in, uint8_t *packet, uint16_t len)
{
#if NWII_EXAMPLE_HCI_DUMP
    hci_dump_embedded_stdout_get_instance()->log_packet(packet_type, in, packet, len);
#endif

    /* Outgoing ACL: handle(2) acl_len(2) l2cap_len(2) cid(2) code(1) id(1) len(2) reason(2) */
    if (packet_type != HCI_ACL_DATA_PACKET || in || len < 14) return;
    if (little_endian_read_16(packet, 6) != L2CAP_CID_SIGNALING) return;
    if (packet[8] != 0x01 || little_endian_read_16(packet, 12) != 0x0002) return; /* Command Reject, invalid CID */
    if (stale_link_handle != HCI_CON_HANDLE_INVALID) return;

    /* Disconnect from a timer rather than from inside the HCI send path */
    stale_link_handle = little_endian_read_16(packet, 0) & 0x0FFF;
    btstack_run_loop_set_timer_handler(&stale_link_timer, &_stale_link_timer_handler);
    btstack_run_loop_set_timer(&stale_link_timer, 1);
    btstack_run_loop_add_timer(&stale_link_timer);
}

static void _stale_link_watch_reset(void)
{
#if NWII_EXAMPLE_HCI_DUMP
    hci_dump_embedded_stdout_get_instance()->reset();
#endif
}

static void _stale_link_watch_log_message(int log_level, const char *format, va_list argptr)
{
#if NWII_EXAMPLE_HCI_DUMP
    hci_dump_embedded_stdout_get_instance()->log_message(log_level, format, argptr);
#else
    (void)log_level; (void)format; (void)argptr;
#endif
}

static const hci_dump_t _stale_link_watch = {
    .reset       = _stale_link_watch_reset,
    .log_packet  = _stale_link_watch_log_packet,
    .log_message = _stale_link_watch_log_message,
};

/* A reloading Wii keeps transmitting but stops acknowledging us, so a normal disconnect waits out
 * the 30 s LMP response timeout. Power-cycling our radio drops the link locally in about a second
 * (BTstack's halting watchdog discards connections the controller cannot close). Because we keep
 * the master role, our 2 s supervision timeout also ends the link on the Wii's side quickly. */
static bool _btc_radio_cycling = false;

static void _nwii_btc_radio_cycle(const char *why)
{
    if (_btc_radio_cycling) return;
    printf("%s; power-cycling the radio to drop the link\n", why);
    _btc_radio_cycling = true;
    _btc_hid_open = false;
    hci_power_control(HCI_POWER_OFF);
}

/* After the Wii closes the HID channels, give a normal disconnect this long before cycling */
static const uint32_t _btc_teardown_ms = 1500;
static btstack_timer_source_t teardown_timer;
static hci_con_handle_t teardown_handle = HCI_CON_HANDLE_INVALID;

static void _teardown_timer_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    /* Only if that same link is still up; a fresh reconnect may already have replaced it */
    if (teardown_handle != HCI_CON_HANDLE_INVALID && teardown_handle == wii_acl_handle)
        _nwii_btc_radio_cycle("Disconnect did not complete");
    teardown_handle = HCI_CON_HANDLE_INVALID;
}

static void _stall_watchdog_handler(btstack_timer_source_t *ts)
{
    if (_btc_hid_open && wii_acl_handle != HCI_CON_HANDLE_INVALID &&
        (btstack_run_loop_get_time_ms() - _btc_last_can_send_ms) > _btc_stall_ms)
    {
        _nwii_btc_radio_cycle("Link stalled (no report sent for 6 s)");
    }

    btstack_run_loop_set_timer(ts, 500);
    btstack_run_loop_add_timer(ts);
}

static void _nwii_btc_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t packet_size)
{
    UNUSED(channel);
    UNUSED(packet_size);
    uint8_t status;
    bd_addr_t addr;

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (packet[0])
    {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_OFF && _btc_radio_cycling)
        {
            wii_acl_handle = HCI_CON_HANDLE_INVALID;
            hid_cid = 0;
            _btc_iac_pending = true; // the controller forgets the IACs on reset
            hci_power_control(HCI_POWER_ON);
            /* The custom address is only applied on the first power-on; without this the radio
             * comes back with its factory address and the Wii refuses the unknown remote. */
            hci_set_bd_addr((uint8_t *)device_mac);
            return;
        }
        if (btstack_event_state_get_state(packet) == HCI_STATE_OFF && _btc_offline)
        {
            wii_acl_handle = HCI_CON_HANDLE_INVALID;
            hid_cid = 0;
            _btc_iac_pending = true;
            printf("Radio off: offline until \"connect\"\n");
            return;
        }
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;

        _btc_radio_cycling = false;
        printf("BTstack up, local address %s\n", bd_addr_to_str(device_mac));
        _nwii_btc_write_iac_task();

        // Discoverable either way so the Wii can SYNC (or temporarily connect) at any time
        gap_discoverable_control(1);

        if (!hid_device_pair_enabled && _nwii_btc_host_saved())
        {
            _nwii_btc_connect_saved_host();
        }
        else
        {
            printf("Discoverable: press SYNC on the Wii\n");
        }
        break;

    case HCI_EVENT_COMMAND_COMPLETE:
    case HCI_EVENT_COMMAND_STATUS:
        if (hci_get_state() == HCI_STATE_WORKING) _nwii_btc_write_iac_task();
        break;

    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, addr);
        printf("Incoming ACL request from %s\n", bd_addr_to_str(addr));
        break;

    case HCI_EVENT_CONNECTION_COMPLETE:
        _btc_page_pending = false;
        btstack_run_loop_remove_timer(&page_stall_timer);
        hci_event_connection_complete_get_bd_addr(packet, addr);
        printf("ACL connection complete: %s status 0x%02X\n", bd_addr_to_str(addr),
               hci_event_connection_complete_get_status(packet));
        if (hci_event_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS)
        {
            wii_acl_handle = hci_event_connection_complete_get_connection_handle(packet);
            _btc_fresh_acl = true;
        }
        break;

    case HCI_EVENT_ROLE_CHANGE:
        if (_btc_page_pending && (hci_event_role_change_get_status(packet) == ERROR_CODE_SUCCESS))
            _btc_page_answered = true;
        printf("Role change: status 0x%02X, now %s\n", hci_event_role_change_get_status(packet),
               hci_event_role_change_get_role(packet) ? "slave" : "master");
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("ACL disconnected, reason 0x%02X\n", hci_event_disconnection_complete_get_reason(packet));
        if (hci_event_disconnection_complete_get_connection_handle(packet) == wii_acl_handle)
            wii_acl_handle = HCI_CON_HANDLE_INVALID;
        if (hci_event_disconnection_complete_get_connection_handle(packet) == teardown_handle)
        {
            btstack_run_loop_remove_timer(&teardown_timer);
            teardown_handle = HCI_CON_HANDLE_INVALID;
        }
        break;

    case HCI_EVENT_PIN_CODE_REQUEST:
    {
        // Wii SYNC pairing is legacy PIN pairing; the PIN is the Wii's address reversed
        uint8_t pin[NWII_HID_PIN_LEN];
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        nwii_hid_make_pin(addr, pin);
        printf("PIN request from %s, replying with its address reversed\n", bd_addr_to_str(addr));
        gap_pin_code_response_binary(addr, pin, NWII_HID_PIN_LEN);
        break;
    }

    case HCI_EVENT_LINK_KEY_REQUEST:
        hci_event_link_key_request_get_bd_addr(packet, addr);
        printf("Link key request from %s\n", bd_addr_to_str(addr));
        break;

    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        hci_event_link_key_request_get_bd_addr(packet, addr);
        printf("Link key stored for %s (type %u)\n", bd_addr_to_str(addr), packet[24]);
        break;

    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        printf("Authentication complete, status 0x%02X\n", hci_event_authentication_complete_get_status(packet));
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        // Should never happen with SSP disabled
        printf("Unexpected SSP confirmation request\n");
        break;

    case HCI_EVENT_HID_META:
        switch (hci_event_hid_meta_get_subevent_code(packet))
        {
        case HID_SUBEVENT_INCOMING_CONNECTION:
            hid_subevent_incoming_connection_get_address(packet, addr);
            printf("HID incoming connection from %s\n", bd_addr_to_str(addr));
            break;

        case HID_SUBEVENT_CONNECTION_OPENED:
            status = hid_subevent_connection_opened_get_status(packet);
            hid_subevent_connection_opened_get_bd_addr(packet, addr);

            if (status)
            {
                printf("HID connection to %s failed, status 0x%02X\n", bd_addr_to_str(addr), status);
                hid_cid = 0;

                /* The Wii took a new link but never answered: it has gone to standby */
                if (_btc_reconnecting && _btc_fresh_acl && (status == L2CAP_CONNECTION_RESPONSE_RESULT_RTX_TIMEOUT))
                {
                    _nwii_btc_wii_standby("Wii took the link but never answered: in standby");
                    return;
                }

                if (!hid_device_pair_enabled && _nwii_btc_host_saved())
                {
                    btstack_run_loop_set_timer_handler(&reconnect_timer, &_reconnect_timer_handler);
                    btstack_run_loop_set_timer(&reconnect_timer, _btc_reconnect_ms);
                    btstack_run_loop_add_timer(&reconnect_timer);
                }
                return;
            }

            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            printf("HID connected to %s\n", bd_addr_to_str(addr));
            _btc_reconnecting = false;

            /* Paired (or reconnected): from now on a dropped link pages this Wii again */
            hid_device_pair_enabled = false;

            if (memcmp(device_storage.host_mac, addr, 6) != 0)
            {
                memcpy(device_storage.host_mac, addr, 6);
                nwii_flash_write((uint8_t *)&device_storage, NWII_STORAGE_SIZE, NWII_STORAGE_PAGE);
                printf("Saved Wii address\n");
            }

            nwii_api_connection_reset();
            _btc_last_can_send_ms = btstack_run_loop_get_time_ms();
            _btc_hid_open = true;
            hid_device_request_can_send_now_event(hid_cid);
            break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
            printf("HID disconnected\n");
            hid_cid = 0;
            _btc_hid_open = false;

            /* Torn down by our own radio cycle (the reconnect happens once BTstack is back up), or
             * by a console "disconnect" */
            if (_btc_radio_cycling || _btc_offline) break;

            /* Quitting a title closes the HID channels but keeps the ACL link, and the Wii's
             * restarted stack will not answer on it; drop it so the reconnect pages fresh. */
            if (wii_acl_handle != HCI_CON_HANDLE_INVALID)
            {
                gap_disconnect(wii_acl_handle);
                teardown_handle = wii_acl_handle;
                btstack_run_loop_remove_timer(&teardown_timer);
                btstack_run_loop_set_timer_handler(&teardown_timer, &_teardown_timer_handler);
                btstack_run_loop_set_timer(&teardown_timer, _btc_teardown_ms);
                btstack_run_loop_add_timer(&teardown_timer);
            }

            /* Starting or quitting a title reloads the Wii's system software; a real remote
             * reconnects on its own, so page the Wii again. */
            _btc_reconnecting = true;
            if (_nwii_btc_host_saved())
            {
                btstack_run_loop_set_timer_handler(&reconnect_timer, &_reconnect_timer_handler);
                btstack_run_loop_set_timer(&reconnect_timer, _btc_reconnect_ms);
                btstack_run_loop_add_timer(&reconnect_timer);
            }
            break;

        case HID_SUBEVENT_CAN_SEND_NOW:
            _btc_last_can_send_ms = btstack_run_loop_get_time_ms();
            if (hid_cid)
            {
                uint32_t current_time_ms = btstack_run_loop_get_time_ms();
                uint32_t time_elapsed = current_time_ms - _btc_last_hid_report_timestamp_ms;

                if (time_elapsed >= _btc_poll_rate_ms)
                {
                    uint8_t report[NWII_INPUT_REPORT_MAX] = {0};
                    uint8_t len = 0;

                    if (nwii_api_generate_inputreport(report, &len))
                    {
                        // Log replies (status / read data / ack); data reports are too frequent
                        if (report[0] < 0x30) _nwii_btc_print_hex("IN ", report, len);
                        _nwii_btc_hid_tunnel(report, len);
                    }

                    _btc_last_hid_report_timestamp_ms = current_time_ms;
                    hid_device_request_can_send_now_event(hid_cid);
                }
                else
                {
                    /* BTstack asked early, so reschedule instead of bursting reports too quickly. */
                    btstack_run_loop_set_timer(&hid_timer, _btc_poll_rate_ms - time_elapsed);
                    btstack_run_loop_set_timer_handler(&hid_timer, &_hid_timer_handler);
                    btstack_run_loop_add_timer(&hid_timer);
                }
            }
            break;

        default:
            break;
        }
        break;

    default:
        break;
    }
}

/* The Wii has gone to standby: stop like a real remote. Not saved, so the next boot connects as
 * usual. */
static void _nwii_btc_wii_standby(const char *why)
{
    printf("%s; staying off (send \"connect\" to try again)\n", why);
    _btc_reconnecting = false;
    _btc_page_pending = false;
    _btc_offline = true;
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_remove_timer(&page_stall_timer);
    hci_power_control(HCI_POWER_OFF);
}

/* Runs on the BTstack thread: drop the saved Wii, its link key and any link, then wait for SYNC */
static void _nwii_btc_forget_wii(void *context)
{
    (void)context;
    printf("Forgetting the paired Wii; press SYNC on the Wii\n");

    hid_device_pair_enabled = true;
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_remove_timer(&teardown_timer);

    if (_nwii_btc_host_saved())
        gap_drop_link_key_for_bd_addr(device_storage.host_mac);

    memset(device_storage.host_mac, 0, sizeof(device_storage.host_mac));
    nwii_flash_write((uint8_t *)&device_storage, NWII_STORAGE_SIZE, NWII_STORAGE_PAGE);

    if (wii_acl_handle != HCI_CON_HANDLE_INVALID)
        gap_disconnect(wii_acl_handle);

    gap_discoverable_control(1);
}

static void _nwii_btc_save_offline(bool offline)
{
    const uint8_t value = offline ? NWII_STORAGE_OFFLINE : 0x00;
    if (device_storage.offline == value) return;
    device_storage.offline = value;
    nwii_flash_write((uint8_t *)&device_storage, NWII_STORAGE_SIZE, NWII_STORAGE_PAGE);
}

static void _nwii_btc_status(void)
{
    static const char *state_names[] = {"off", "initializing", "working", "halting", "sleeping", "falling asleep"};
    const HCI_STATE state = hci_get_state();

    printf("Radio: %s%s, ACL: %s, HID: %s, saved Wii: %s%s\n",
           (state < sizeof(state_names) / sizeof(state_names[0])) ? state_names[state] : "?",
           _btc_offline ? " (offline)" : "",
           (wii_acl_handle != HCI_CON_HANDLE_INVALID) ? "up" : "down",
           _btc_hid_open ? "open" : "closed",
           _nwii_btc_host_saved() ? bd_addr_to_str(device_storage.host_mac) : "none",
           hid_device_pair_enabled ? ", waiting for SYNC" : "");
}

static volatile uint32_t _btc_requests = 0;

/* Runs on the BTstack thread */
static void _nwii_btc_run_requests(void *context)
{
    (void)context;

    const uint32_t save = save_and_disable_interrupts();
    const uint32_t requests = _btc_requests;
    _btc_requests = 0;
    restore_interrupts(save);

    if (requests & NWII_BTC_REQUEST_DISCONNECT)
    {
        printf("Disconnecting; radio off\n");
        _btc_offline = true;
        _btc_hid_open = false;
        _nwii_btc_save_offline(true);
        btstack_run_loop_remove_timer(&reconnect_timer);
        btstack_run_loop_remove_timer(&teardown_timer);
        /* Powering off closes the link with a normal disconnect first */
        hci_power_control(HCI_POWER_OFF);
    }

    if (requests & NWII_BTC_REQUEST_CONNECT)
    {
        _nwii_btc_save_offline(false);
        if (_btc_offline || hci_get_state() == HCI_STATE_OFF)
        {
            printf("Connecting; radio on\n");
            _btc_offline = false;
            hci_power_control(HCI_POWER_ON);
            hci_set_bd_addr((uint8_t *)device_mac);
        }
        else if (!hid_cid && _nwii_btc_host_saved())
        {
            _nwii_btc_connect_saved_host();
        }
        else
        {
            printf("Already connected or connecting\n");
        }
    }

    if (requests & NWII_BTC_REQUEST_CYCLE)
    {
        if (_btc_offline || hci_get_state() != HCI_STATE_WORKING)
            printf("Radio is not up; use \"connect\"\n");
        else
            _nwii_btc_radio_cycle("Console request");
    }

    if (requests & NWII_BTC_REQUEST_FORGET)
    {
        _nwii_btc_forget_wii(NULL);
        if (_btc_offline)
        {
            _btc_offline = false;
            _nwii_btc_save_offline(false);
            hci_power_control(HCI_POWER_ON);
            hci_set_bd_addr((uint8_t *)device_mac);
        }
    }

    if (requests & NWII_BTC_REQUEST_STATUS)
    {
        _nwii_btc_status();
    }
}

void nwii_btc_request(nwii_btc_request_t request)
{
    static btstack_context_callback_registration_t registration = {
        .callback = &_nwii_btc_run_requests,
    };

    const uint32_t save = save_and_disable_interrupts();
    const bool idle = (_btc_requests == 0);
    _btc_requests |= request;
    restore_interrupts(save);

    // One pending callback carries every request made before it runs
    if (idle) btstack_run_loop_execute_on_main_thread(&registration);
}

void nwii_btc_enter(const uint8_t device_mac[6], bool pairing_mode)
{
    /* Bring up the Pico W wireless stack before any BTstack objects are configured. */
    if (cyw43_arch_init())
    {
        printf("cyw43_arch_init failed\n");
        return;
    }

    hci_dump_init(&_stale_link_watch);

    /* GAP identity. The Wii only does legacy PIN pairing, so Secure Simple Pairing is off, and it
     * authenticates on its own terms, so we never ask for security ourselves. */
    gap_ssp_set_enable(0);
    gap_set_security_level(LEVEL_0);
    gap_set_bondable_mode(1);
    gap_set_class_of_device(NWII_HID_CLASS_OF_DEVICE);
    gap_set_local_name(nwii_hid_get_device_name());
    /* Let the Wii take the master role, as real remotes do: with other remotes connected, or once
     * a game is running, the Wii hangs up on a remote that refuses the role switch. */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    gap_set_link_supervision_timeout(NWII_HID_LINK_SUPERVISION_TIMEOUT);
    gap_set_page_timeout(NWII_BTC_PAGE_TIMEOUT);

    btstack_run_loop_set_timer_handler(&stall_watchdog_timer, &_stall_watchdog_handler);
    btstack_run_loop_set_timer(&stall_watchdog_timer, 500);
    btstack_run_loop_add_timer(&stall_watchdog_timer);

    hci_set_chipset(btstack_chipset_cyw43_instance());

    l2cap_init();
    sm_init();
    sdp_init();

    /* The Wii checks the HID record, so serve a real remote's record verbatim. It carries its own
     * record handle, so register it before allocating the PnP record's handle. */
    const uint8_t *record = NULL;
    uint16_t record_len = 0;
    nwii_hid_get_sdp_record(&record, &record_len);
    memcpy(hid_service_buffer, record, record_len);
    printf("SDP HID record: 0x%02X\n", sdp_register_service(hid_service_buffer));

    device_id_create_sdp_record(pnp_service_buffer, sdp_create_service_record_handle(), DEVICE_ID_VENDOR_ID_SOURCE_USB,
                                NWII_HID_VID, NWII_HID_PID, 0x0100);
    printf("SDP PnP record: 0x%02X\n", sdp_register_service(pnp_service_buffer));

    const uint8_t *descriptor = NULL;
    uint16_t descriptor_len = 0;
    nwii_hid_get_report_descriptor(&descriptor, &descriptor_len);
    hid_device_init(0, descriptor_len, descriptor);
    hid_device_accept_truncated_hid_reports(true);

    /* Funnel both raw HCI events and HID meta-events through the same state machine. */
    hci_event_callback_registration.callback = &_nwii_btc_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    hid_device_register_packet_handler(&_nwii_btc_packet_handler);
    hid_device_register_report_data_callback(&_nwii_btc_outputreport_handler);
    hid_device_register_set_report_callback(&_nwii_btc_setreport_handler);

    hid_device_pair_enabled = pairing_mode;

    /* A console "disconnect" is kept across reboots so the Pico stays off the Wii until told */
    /* Give a console command sent right at boot (e.g. "disconnect") a moment to arrive, so a fresh
     * flash can come up without touching the Wii */
    for (int i = 0; i < 30; i++)
    {
        nwii_console_task();
        sleep_ms(10);
    }

    _btc_offline = ((device_storage.offline == NWII_STORAGE_OFFLINE) || (_btc_requests & NWII_BTC_REQUEST_DISCONNECT)) &&
                   !pairing_mode;
    if (_btc_offline)
    {
        printf("Offline (console \"disconnect\"); send \"connect\" to go back on the Wii\n");
    }
    else
    {
        hci_power_control(HCI_POWER_ON);
        hci_set_bd_addr((uint8_t *)device_mac);
    }

    /* Main loop stays tiny: flash writes happen here, outside the Bluetooth callbacks, and the USB
     * serial console takes commands (send "help"). */
    for (;;)
    {
        nwii_flash_task();
        nwii_console_task();

        sleep_ms(1);
    }
}
