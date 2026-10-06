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

#include "pico/cyw43_arch.h"
#include "pico/btstack_chipset_cyw43.h"
#include "btstack.h"

#if NWII_EXAMPLE_HCI_DUMP
#include "hci_dump.h"
#include "hci_dump_embedded_stdout.h"
#endif

#include "nwii_lib.h"

/* A real remote reports at ~100 Hz. */
static const uint32_t _btc_poll_rate_ms = 10;
static uint32_t _btc_last_hid_report_timestamp_ms = 0;
static btstack_timer_source_t hid_timer;

/* A paired remote connects to the Wii, never the reverse, so keep paging until it answers. */
static const uint32_t _btc_reconnect_ms = 2000;
static btstack_timer_source_t reconnect_timer;

static bool hid_device_pair_enabled = false;

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

static void _nwii_btc_connect_saved_host(void)
{
    printf("Paging saved Wii %s\n", bd_addr_to_str(device_storage.host_mac));
    uint8_t status = hid_device_connect(device_storage.host_mac, &hid_cid);
    if (status) printf("hid_device_connect error 0x%02X\n", status);
}

static void _reconnect_timer_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    if (!hid_cid) _nwii_btc_connect_saved_host();
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
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;

        printf("BTstack up, local address %s\n", bd_addr_to_str(device_mac));

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

    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, addr);
        printf("Incoming ACL request from %s\n", bd_addr_to_str(addr));
        break;

    case HCI_EVENT_CONNECTION_COMPLETE:
        hci_event_connection_complete_get_bd_addr(packet, addr);
        printf("ACL connection complete: %s status 0x%02X\n", bd_addr_to_str(addr),
               hci_event_connection_complete_get_status(packet));
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("ACL disconnected, reason 0x%02X\n", hci_event_disconnection_complete_get_reason(packet));
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

            if (memcmp(device_storage.host_mac, addr, 6) != 0)
            {
                memcpy(device_storage.host_mac, addr, 6);
                nwii_flash_write((uint8_t *)&device_storage, NWII_STORAGE_SIZE, NWII_STORAGE_PAGE);
                printf("Saved Wii address\n");
            }

            nwii_api_connection_reset();
            hid_device_request_can_send_now_event(hid_cid);
            break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
            printf("HID disconnected\n");
            hid_cid = 0;
            break;

        case HID_SUBEVENT_CAN_SEND_NOW:
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

void nwii_btc_enter(const uint8_t device_mac[6], bool pairing_mode)
{
    /* Bring up the Pico W wireless stack before any BTstack objects are configured. */
    if (cyw43_arch_init())
    {
        printf("cyw43_arch_init failed\n");
        return;
    }

#if NWII_EXAMPLE_HCI_DUMP
    hci_dump_init(hci_dump_embedded_stdout_get_instance());
#endif

    /* GAP identity. The Wii only does legacy PIN pairing, so Secure Simple Pairing is off, and it
     * authenticates on its own terms, so we never ask for security ourselves. */
    gap_ssp_set_enable(0);
    gap_set_security_level(LEVEL_0);
    gap_set_bondable_mode(1);
    gap_set_class_of_device(NWII_HID_CLASS_OF_DEVICE);
    gap_set_local_name(nwii_hid_get_device_name());
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);

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

    hci_power_control(HCI_POWER_ON);
    hci_set_bd_addr((uint8_t *)device_mac);

    /* Main loop stays tiny: flash writes happen here, outside the Bluetooth callbacks. */
    for (;;)
    {
        nwii_flash_task();
        sleep_ms(1);
    }
}
