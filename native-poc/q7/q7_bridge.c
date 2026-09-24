// Q7 bridge: the PC acts as a hands-free unit for a basic phone (HFP HF role)
// and as an audio gateway for a Bluetooth headset (HFP AG role) at the same time.
// Call audio (CVSD, 8 kHz) flows phone <-> PC <-> headset, and a copy of both
// directions is offered to a local listener app via call_tap (named pipe).
// If no headset is connected, the PC's default communications speaker/mic is used.
//
// Security model (see ../../SECURITY-NOTES.md):
//  - Not discoverable and not bondable except during an explicit pairing window
//    (key 'p', 120 s) or while connecting to a headset the user asked for (key 'h').
//  - Incoming connections, SSP confirmations and PIN requests are accepted only
//    from the saved phone/headset, or inside those windows.
//  - Only CVSD is offered (no SBC/AAC/LC3 decoders, no A2DP/AVRCP).
//  - No packet log unless Q7_DEBUG_PKLG is set, and then without link keys/PINs.
// Explicit headers instead of btstack.h, which pulls in codec headers we don't build.
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "btstack_tlv.h"
#include "hci.h"
#include "hci_dump.h"
#include "gap.h"
#include "l2cap.h"
#include "classic/rfcomm.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "classic/hfp.h"
#include "classic/hfp_ag.h"
#include "classic/hfp_hf.h"
#include "btstack_run_loop_windows.h"
#include "btstack_stdin.h"
#include "btstack_stdin_windows.h"
#include "btstack_tlv_windows.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/btstack_cvsd_plc.h"
#include "hci_transport_usb.h"
#include "hci_dump_windows_fs.h"
#include "audio/audio_bridge.h"
#include "call_tap.h"
#include "pcm_fifo.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HF_CHANNEL 1
#define AG_CHANNEL 2
#define PAIRING_WINDOW_MS 120000
#define RECONNECT_MS 20000
#define STARTUP_TIMEOUT_TICKS 150 /* x 200 ms = 30 s */

typedef enum { LINK_PHONE = 0, LINK_HEADSET = 1 } link_t;

static FILE * log_file;
static void say(const char * format, ...) {
    va_list args;
    va_start(args, format); vprintf(format, args); va_end(args);
    if (log_file) { va_start(args, format); vfprintf(log_file, format, args); va_end(args); fflush(log_file); }
}

// ---- persistent device roles -------------------------------------------------
static bd_addr_t phone_addr, headset_addr, pending_headset;
static int have_phone, have_headset, have_pending;

static void devices_save(void) {
    FILE * f = fopen("devices.txt", "w");
    if (!f) { say("[CFG] Cannot write devices.txt\n"); return; }
    if (have_phone) fprintf(f, "phone %s\n", bd_addr_to_str(phone_addr));
    if (have_headset) fprintf(f, "headset %s\n", bd_addr_to_str(headset_addr));
    fclose(f);
}
static void devices_load(void) {
    FILE * f = fopen("devices.txt", "r");
    if (!f) return;
    char role[16], addr[32];
    while (fscanf(f, "%15s %31s", role, addr) == 2) {
        if (strcmp(role, "phone") == 0 && sscanf_bd_addr(addr, phone_addr)) have_phone = 1;
        if (strcmp(role, "headset") == 0 && sscanf_bd_addr(addr, headset_addr)) have_headset = 1;
    }
    fclose(f);
}

// ---- access control ----------------------------------------------------------
static int pairing_open;
static btstack_timer_source_t pairing_timer;

static int known(const bd_addr_t addr) {
    return (have_phone && bd_addr_cmp(addr, phone_addr) == 0) ||
           (have_headset && bd_addr_cmp(addr, headset_addr) == 0) ||
           (have_pending && bd_addr_cmp(addr, pending_headset) == 0);
}
static int allowed(const bd_addr_t addr) { return pairing_open || known(addr); }

static int connection_filter(bd_addr_t addr, hci_link_type_t link_type) {
    (void)link_type;
    if (allowed(addr)) return 1;
    say("[SEC] Rejected connection from unknown device %s\n", bd_addr_to_str(addr));
    return 0;
}

static void update_bondable(void) { gap_set_bondable_mode(pairing_open || have_pending); }

static void pairing_close(btstack_timer_source_t * ts) {
    (void)ts;
    if (!pairing_open) return;
    pairing_open = 0;
    gap_discoverable_control(0);
    update_bondable();
    say("[SEC] Pairing window closed; only saved devices can connect\n");
}
static void pairing_start(void) {
    pairing_open = 1;
    gap_discoverable_control(1);
    update_bondable();
    btstack_run_loop_remove_timer(&pairing_timer);
    btstack_run_loop_set_timer_handler(&pairing_timer, pairing_close);
    btstack_run_loop_set_timer(&pairing_timer, PAIRING_WINDOW_MS);
    btstack_run_loop_add_timer(&pairing_timer);
    say("[SEC] Pairing open for 120 s. On the Q7: Bluetooth > search > \"Q7 Bridge\" > pair.\n");
}

// ---- connection state ----------------------------------------------------------
static hci_con_handle_t phone_acl = HCI_CON_HANDLE_INVALID, headset_acl = HCI_CON_HANDLE_INVALID;
static hci_con_handle_t phone_sco = HCI_CON_HANDLE_INVALID, headset_sco = HCI_CON_HANDLE_INVALID;
static int phone_connecting, headset_connecting;
static int phone_call, phone_callsetup, ag_ringing, ag_call;
static int speaker_fallback;

// ---- audio -------------------------------------------------------------------------
static btstack_cvsd_plc_state_t plc[2];
static pcm_fifo_t to_headset, to_phone;
static unsigned sco_packets[2], sco_dropped[2];

static void speaker_fallback_set(int on) {
    if (on == speaker_fallback) return;
    speaker_fallback = on;
    if (on) { audio_start(8000); say("[AUDIO] No headset audio: using the PC speaker/microphone\n"); }
    else audio_stop();
}

static void send_sco(link_t link, const int16_t * samples, unsigned count) {
    const hci_con_handle_t handle = link == LINK_PHONE ? phone_sco : headset_sco;
    if (handle == HCI_CON_HANDLE_INVALID) return;
    if (!hci_can_send_sco_packet_now_for_con_handle(handle)) { sco_dropped[link]++; return; }
    hci_reserve_packet_buffer();
    uint8_t * packet = hci_get_outgoing_packet_buffer();
    little_endian_store_16(packet, 0, handle);
    packet[2] = (uint8_t)(count * 2);
    for (unsigned i = 0; i < count; ++i) little_endian_store_16(packet, 3 + 2 * i, (uint16_t)samples[i]);
    hci_send_sco_packet_buffer(3 + (int)count * 2);
}

static void sco_packet(uint8_t type, uint16_t channel, uint8_t * packet, uint16_t size) {
    (void)channel;
    if (type != HCI_SCO_DATA_PACKET || size < 3 || size != packet[2] + 3u) return;
    const hci_con_handle_t handle = little_endian_read_16(packet, 0) & 0x0fff;
    link_t link;
    if (handle == phone_sco && phone_sco != HCI_CON_HANDLE_INVALID) link = LINK_PHONE;
    else if (handle == headset_sco && headset_sco != HCI_CON_HANDLE_INVALID) link = LINK_HEADSET;
    else return;
    const unsigned bytes = packet[2];
    if (bytes % 2 || bytes / 2 > 128) return; // 16-bit linear PCM only (voice setting 0x0060)
    const unsigned count = bytes / 2;
    int16_t input[128], audio[128], reply[128];
    for (unsigned i = 0; i < count; ++i) input[i] = (int16_t)little_endian_read_16(packet, 3 + 2 * i);
    // Packet-loss concealment in chunks the PLC supports (at most CVSD_FS samples).
    const bool bad = (packet[1] & 0x30) != 0;
    for (unsigned done = 0; done < count; done += CVSD_FS) {
        const unsigned n = count - done < CVSD_FS ? count - done : CVSD_FS;
        btstack_cvsd_plc_process_data(&plc[link], bad, input + done, (uint16_t)n, audio + done);
    }
    if (++sco_packets[link] == 1) say("[SCO] First %s audio packet (%u samples)\n", link == LINK_PHONE ? "phone" : "headset", count);

    if (link == LINK_PHONE) {
        tap_push(0, audio, count);
        if (headset_sco != HCI_CON_HANDLE_INVALID) pcm_fifo_push(&to_headset, audio, count);
        else if (speaker_fallback) audio_render_write(audio, count);
        // Reply to the phone with the same amount of the user's voice (paces TX by RX).
        if (headset_sco != HCI_CON_HANDLE_INVALID) pcm_fifo_pull(&to_phone, reply, count);
        else if (speaker_fallback) { audio_capture_read(reply, count); tap_push(1, reply, count); }
        else memset(reply, 0, count * sizeof(int16_t));
        send_sco(LINK_PHONE, reply, count);
    } else {
        tap_push(1, audio, count);
        pcm_fifo_push(&to_phone, audio, count);
        pcm_fifo_pull(&to_headset, reply, count);
        send_sco(LINK_HEADSET, reply, count);
    }
}

// ---- phone side (we are the hands-free unit) ----------------------------------------
static void mirror_call_state(const char * name, int status) {
    if (strcmp(name, "callsetup") == 0) {
        phone_callsetup = status;
        if (status == 1 && headset_acl != HCI_CON_HANDLE_INVALID && !ag_ringing && !ag_call) {
            ag_ringing = 1; hfp_ag_incoming_call(); say("[CALL] Incoming call: headset rings\n");
        } else if (status == 0 && ag_ringing && !phone_call) {
            ag_ringing = 0; hfp_ag_call_dropped(); say("[CALL] Call not answered\n");
        }
    } else if (strcmp(name, "call") == 0) {
        phone_call = status;
        if (status == 1) {
            if (ag_ringing) { ag_ringing = 0; ag_call = 1; hfp_ag_answer_incoming_call(); }
            say("[CALL] Call active\n");
        } else {
            if (ag_call) { ag_call = 0; hfp_ag_terminate_call(); }
            say("[CALL] Call ended\n");
        }
    }
}

static void phone_event(uint8_t type, uint16_t channel, uint8_t * event, uint16_t size) {
    (void)channel; (void)size;
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(event) != HCI_EVENT_HFP_META) return;
    bd_addr_t addr;
    uint8_t status;
    switch (hci_event_hfp_meta_get_subevent_code(event)) {
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED:
            phone_connecting = 0;
            status = hfp_subevent_service_level_connection_established_get_status(event);
            hfp_subevent_service_level_connection_established_get_bd_addr(event, addr);
            if (status) { say("[PHONE] Connection to %s failed (0x%02x)\n", bd_addr_to_str(addr), status); break; }
            phone_acl = hfp_subevent_service_level_connection_established_get_acl_handle(event);
            if (!have_phone || bd_addr_cmp(addr, phone_addr) != 0) { bd_addr_copy(phone_addr, addr); have_phone = 1; devices_save(); }
            say("[PHONE] Connected: %s\n", bd_addr_to_str(addr));
            break;
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
            phone_acl = HCI_CON_HANDLE_INVALID; phone_sco = HCI_CON_HANDLE_INVALID;
            phone_call = phone_callsetup = 0;
            speaker_fallback_set(0);
            if (ag_ringing) { ag_ringing = 0; hfp_ag_call_dropped(); }
            if (ag_call) { ag_call = 0; hfp_ag_terminate_call(); }
            if (headset_sco != HCI_CON_HANDLE_INVALID) hfp_ag_release_audio_connection(headset_acl);
            say("[PHONE] Disconnected\n");
            break;
        case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED:
            status = hfp_subevent_audio_connection_established_get_status(event);
            if (status) { say("[PHONE] Call audio failed (0x%02x)\n", status); break; }
            phone_sco = hfp_subevent_audio_connection_established_get_sco_handle(event);
            btstack_cvsd_plc_init(&plc[LINK_PHONE]);
            pcm_fifo_reset(&to_headset); pcm_fifo_reset(&to_phone);
            sco_packets[LINK_PHONE] = sco_dropped[LINK_PHONE] = 0;
            say("[PHONE] Call audio connected (codec %u)\n", hfp_subevent_audio_connection_established_get_negotiated_codec(event));
            if (headset_acl != HCI_CON_HANDLE_INVALID) {
                if (headset_sco == HCI_CON_HANDLE_INVALID) hfp_ag_establish_audio_connection(headset_acl);
            } else speaker_fallback_set(1);
            break;
        case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED:
            phone_sco = HCI_CON_HANDLE_INVALID;
            speaker_fallback_set(0);
            if (headset_sco != HCI_CON_HANDLE_INVALID) hfp_ag_release_audio_connection(headset_acl);
            say("[PHONE] Call audio released (dropped TX packets: %u)\n", sco_dropped[LINK_PHONE]);
            break;
        case HFP_SUBEVENT_AG_INDICATOR_STATUS_CHANGED:
            mirror_call_state(hfp_subevent_ag_indicator_status_changed_get_indicator_name(event),
                hfp_subevent_ag_indicator_status_changed_get_indicator_status(event));
            break;
        default: break;
    }
}

// ---- headset side (we are the audio gateway) ------------------------------------------
static void headset_event(uint8_t type, uint16_t channel, uint8_t * event, uint16_t size) {
    (void)channel; (void)size;
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(event) != HCI_EVENT_HFP_META) return;
    bd_addr_t addr;
    uint8_t status;
    switch (hci_event_hfp_meta_get_subevent_code(event)) {
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED:
            headset_connecting = 0;
            status = hfp_subevent_service_level_connection_established_get_status(event);
            hfp_subevent_service_level_connection_established_get_bd_addr(event, addr);
            if (have_pending && bd_addr_cmp(addr, pending_headset) == 0) { have_pending = 0; update_bondable(); }
            if (status) { say("[HEADSET] Connection to %s failed (0x%02x)\n", bd_addr_to_str(addr), status); break; }
            headset_acl = hfp_subevent_service_level_connection_established_get_acl_handle(event);
            if (!have_headset || bd_addr_cmp(addr, headset_addr) != 0) { bd_addr_copy(headset_addr, addr); have_headset = 1; devices_save(); }
            say("[HEADSET] Connected: %s\n", bd_addr_to_str(addr));
            if (phone_sco != HCI_CON_HANDLE_INVALID) hfp_ag_establish_audio_connection(headset_acl);
            break;
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
            headset_acl = HCI_CON_HANDLE_INVALID; headset_sco = HCI_CON_HANDLE_INVALID;
            ag_ringing = ag_call = 0;
            if (phone_sco != HCI_CON_HANDLE_INVALID) speaker_fallback_set(1);
            say("[HEADSET] Disconnected\n");
            break;
        case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED:
            status = hfp_subevent_audio_connection_established_get_status(event);
            if (status) {
                say("[HEADSET] Audio failed (0x%02x)\n", status);
                if (phone_sco != HCI_CON_HANDLE_INVALID) speaker_fallback_set(1);
                break;
            }
            headset_sco = hfp_subevent_audio_connection_established_get_sco_handle(event);
            btstack_cvsd_plc_init(&plc[LINK_HEADSET]);
            pcm_fifo_reset(&to_headset); pcm_fifo_reset(&to_phone);
            sco_packets[LINK_HEADSET] = sco_dropped[LINK_HEADSET] = 0;
            speaker_fallback_set(0);
            say("[HEADSET] Audio connected\n");
            break;
        case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED:
            headset_sco = HCI_CON_HANDLE_INVALID;
            if (phone_sco != HCI_CON_HANDLE_INVALID) speaker_fallback_set(1);
            say("[HEADSET] Audio released\n");
            break;
        case HFP_SUBEVENT_CALL_ANSWERED: // headset button while ringing
            ag_ringing = 0; ag_call = 1;
            if (phone_callsetup == 1 && !phone_call && phone_acl != HCI_CON_HANDLE_INVALID) hfp_hf_answer_incoming_call(phone_acl);
            break;
        case HFP_SUBEVENT_CALL_TERMINATED: // headset button during a call
            ag_ringing = ag_call = 0;
            if ((phone_call || phone_callsetup) && phone_acl != HCI_CON_HANDLE_INVALID) hfp_hf_terminate_call(phone_acl);
            break;
        default: break;
    }
}

// ---- GAP / pairing -------------------------------------------------------------------
static bd_addr_t candidate;
static int have_candidate, searching;

static void gap_event(uint8_t type, uint16_t channel, uint8_t * packet, uint16_t size);

static void connect_known(void) {
    if (have_phone && phone_acl == HCI_CON_HANDLE_INVALID && !phone_connecting) {
        phone_connecting = hfp_hf_establish_service_level_connection(phone_addr) == ERROR_CODE_SUCCESS;
    }
    if (have_headset && headset_acl == HCI_CON_HANDLE_INVALID && !headset_connecting) {
        headset_connecting = hfp_ag_establish_service_level_connection(headset_addr) == ERROR_CODE_SUCCESS;
    }
}

static int working, stopping;
static volatile LONG quit_requested;
static unsigned ticks, stop_ticks, reconnect_ticks;
static btstack_timer_source_t tick_timer;
static btstack_tlv_windows_t tlv_context;

static void begin_shutdown(void) {
    if (stopping) return;
    stopping = 1;
    say("[APP] Stopping...\n");
    speaker_fallback_set(0);
    tap_close();
    if (working) hci_power_control(HCI_POWER_OFF); else exit(0);
}

static void tick(btstack_timer_source_t * ts) {
    if (InterlockedCompareExchange(&quit_requested, 0, 0)) begin_shutdown();
    if (!working && !stopping && ++ticks > STARTUP_TIMEOUT_TICKS) {
        say("[HCI] Controller did not start within 30 s (firmware not loaded, or wrong USB device)\n");
        exit(2);
    }
    if (stopping && ++stop_ticks > 25) exit(0); // shutdown watchdog: 5 s
    if (working && !stopping && ++reconnect_ticks >= RECONNECT_MS / 200) { reconnect_ticks = 0; connect_known(); }
    btstack_run_loop_set_timer(ts, 200);
    btstack_run_loop_add_timer(ts);
}

static void addr_or_none(char * out, size_t size, int have, const bd_addr_t addr) {
    // bd_addr_to_str() returns one static buffer, so copy before formatting two addresses.
    strcpy_s(out, size, have ? bd_addr_to_str(addr) : "(none)");
}
static void print_status(void) {
    char phone[20], headset[20];
    addr_or_none(phone, sizeof(phone), have_phone, phone_addr);
    addr_or_none(headset, sizeof(headset), have_headset, headset_addr);
    say("[STATUS] phone=%s%s headset=%s%s pairing=%s call=%d audio: phone=%s headset=%s speakerFallback=%d\n",
        phone, phone_acl != HCI_CON_HANDLE_INVALID ? " [connected]" : "",
        headset, headset_acl != HCI_CON_HANDLE_INVALID ? " [connected]" : "",
        pairing_open ? "open" : "closed", phone_call,
        phone_sco != HCI_CON_HANDLE_INVALID ? "on" : "off", headset_sco != HCI_CON_HANDLE_INVALID ? "on" : "off", speaker_fallback);
}

static void key(char c) {
    if (stopping) return;
    switch (c) {
        case 'p': pairing_start(); break;
        case 'h':
            if (searching) break;
            searching = 1; have_candidate = 0;
            say("[HEADSET] Put the headset in pairing mode now. Searching for 10 s...\n");
            gap_inquiry_start(8);
            break;
        case 'c': connect_known(); say("[APP] Reconnecting saved devices\n"); break;
        case 's': print_status(); break;
        case 'q': begin_shutdown(); break;
        default:
            say("Keys: p = pair the phone, h = connect a headset, c = reconnect, s = status, q = quit\n");
            break;
    }
}
static void ctrl_c(void) { begin_shutdown(); }

static BOOL WINAPI console_handler(DWORD kind) {
    (void)kind;
    InterlockedExchange(&quit_requested, 1); // picked up by tick() on the BTstack thread
    Sleep(5000); // give the run loop time to power down cleanly before Windows ends us
    return TRUE;
}

static void gap_event(uint8_t type, uint16_t channel, uint8_t * packet, uint16_t size) {
    (void)channel; (void)size;
    if (type != HCI_EVENT_PACKET) return;
    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING && !working) {
                working = 1;
                gap_local_bd_addr(addr);
                say("[HCI] Controller ready. Local address %s\n", bd_addr_to_str(addr));
                say("Keys: p = pair the phone, h = connect a headset, c = reconnect, s = status, q = quit\n");
                tap_open();
                connect_known();
            } else if (btstack_event_state_get_state(packet) == HCI_STATE_OFF && stopping) {
                btstack_tlv_windows_deinit(&tlv_context);
                say("[APP] Stopped\n");
                exit(0);
            }
            break;
        case GAP_EVENT_INQUIRY_RESULT: {
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            const uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
            const int audio_device = ((cod >> 8) & 0x1f) == 0x04; // major class Audio/Video
            char name[64] = "";
            if (gap_event_inquiry_result_get_name_available(packet)) {
                const int len = gap_event_inquiry_result_get_name_len(packet) < 63 ? gap_event_inquiry_result_get_name_len(packet) : 63;
                memcpy(name, gap_event_inquiry_result_get_name(packet), len); name[len] = 0;
            }
            say("[SCAN] %s %s%s\n", bd_addr_to_str(addr), name, audio_device ? " (audio)" : "");
            if (audio_device && !have_candidate && !(have_phone && bd_addr_cmp(addr, phone_addr) == 0)) {
                bd_addr_copy(candidate, addr); have_candidate = 1;
            }
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE:
            searching = 0;
            if (!have_candidate) { say("[HEADSET] No headset in pairing mode was found. Press h to try again.\n"); break; }
            bd_addr_copy(pending_headset, candidate); have_pending = 1; update_bondable();
            say("[HEADSET] Connecting to %s...\n", bd_addr_to_str(candidate));
            if (hfp_ag_establish_service_level_connection(candidate) != ERROR_CODE_SUCCESS) {
                have_pending = 0; update_bondable(); say("[HEADSET] Could not start the connection\n");
            } else headset_connecting = 1;
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            if (allowed(addr)) gap_ssp_confirmation_response(addr);
            else { gap_ssp_confirmation_negative(addr); say("[SEC] Refused pairing from %s\n", bd_addr_to_str(addr)); }
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            if (allowed(addr)) gap_pin_code_response(addr, "0000");
            else { gap_pin_code_negative(addr); say("[SEC] Refused PIN pairing from %s\n", bd_addr_to_str(addr)); }
            break;
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            say("[SEC] Pairing finished (status 0x%02x)\n", hci_event_simple_pairing_complete_get_status(packet));
            break;
        default: break;
    }
}

// ---- optional packet log without secrets ------------------------------------------------
static hci_dump_t filtered_dump;
static void log_packet_without_secrets(uint8_t type, uint8_t in, uint8_t * packet, uint16_t len) {
    if (type == HCI_SCO_DATA_PACKET || type == HCI_ACL_DATA_PACKET) return;
    if (type == HCI_EVENT_PACKET && len > 0 && packet[0] == HCI_EVENT_LINK_KEY_NOTIFICATION) return;
    if (type == HCI_COMMAND_DATA_PACKET && len >= 2) {
        const uint16_t opcode = little_endian_read_16(packet, 0);
        if (opcode == 0x040B /* Link_Key_Request_Reply */ || opcode == 0x040D /* PIN_Code_Request_Reply */) return;
    }
    hci_dump_windows_fs_get_instance()->log_packet(type, in, packet, len);
}

void hal_led_toggle(void) {}

int main(void) {
    HANDLE singleton = CreateMutexW(NULL, FALSE, L"Local\\Q7Bridge.Engine");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) { printf("Q7 bridge is already running\n"); return 73; }
    setvbuf(stdout, NULL, _IONBF, 0);
    log_file = fopen("q7-bridge.log", "a");
    say("\n[APP] Q7 bridge starting\n");
    devices_load();
    SetConsoleCtrlHandler(console_handler, TRUE);

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());
    if (getenv("Q7_DEBUG_PKLG")) {
        hci_dump_windows_fs_open("hci_dump.pklg", HCI_DUMP_PACKETLOGGER);
        filtered_dump = *hci_dump_windows_fs_get_instance();
        filtered_dump.log_packet = log_packet_without_secrets;
        hci_dump_init(&filtered_dump);
        say("[APP] Debug packet log enabled (link keys and PINs are not logged)\n");
    }
    hci_init(hci_transport_usb_instance(), NULL);
    const btstack_tlv_t * tlv = btstack_tlv_windows_init_instance(&tlv_context, "link-keys.tlv");
    btstack_tlv_set_instance(tlv, &tlv_context);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv, &tlv_context));

    l2cap_init();
    rfcomm_init();
    sdp_init();

    static const uint8_t codecs[] = {HFP_CODEC_CVSD};
    static uint8_t hf_record[200], ag_record[200];
    static const hfp_ag_indicator_t indicators[] = {
        {1, "service",   0, 1, 1, 0, 0, 0},
        {2, "call",      0, 1, 0, 1, 1, 0},
        {3, "callsetup", 0, 3, 0, 1, 1, 0},
        {4, "battchg",   0, 5, 3, 0, 0, 0},
        {5, "signal",    0, 5, 5, 0, 1, 0},
        {6, "roam",      0, 1, 0, 0, 1, 0},
        {7, "callheld",  0, 2, 0, 1, 1, 0},
    };

    // Phone side: we are the hands-free unit (CVSD only).
    const uint16_t hf_features = (1 << HFP_HFSF_ESCO_S4);
    if (hfp_hf_init(HF_CHANNEL) != ERROR_CODE_SUCCESS) { say("[HFP] HF init failed\n"); return 3; }
    hfp_hf_init_supported_features(hf_features);
    hfp_hf_init_codecs(1, codecs);
    hfp_hf_register_packet_handler(phone_event);
    hfp_hf_create_sdp_record_with_codecs(hf_record, sdp_create_service_record_handle(), HF_CHANNEL,
        "Q7 Bridge Hands-Free", hf_features, 1, codecs);

    // Headset side: we are the audio gateway (CVSD only, no in-band ring tone).
    const uint16_t ag_features = (1 << HFP_AGSF_ESCO_S4) | (1 << HFP_AGSF_ABILITY_TO_REJECT_A_CALL) |
        (1 << HFP_AGSF_EXTENDED_ERROR_RESULT_CODES);
    hfp_ag_init(AG_CHANNEL);
    hfp_ag_init_supported_features(ag_features);
    hfp_ag_init_codecs(1, codecs);
    hfp_ag_init_ag_indicators((int)(sizeof(indicators) / sizeof(indicators[0])), indicators);
    hfp_ag_set_use_in_band_ring_tone(0);
    hfp_ag_register_packet_handler(headset_event);
    hfp_ag_create_sdp_record_with_codecs(ag_record, sdp_create_service_record_handle(), AG_CHANNEL,
        "Q7 Bridge Audio Gateway", 1, ag_features, 1, codecs);

    if (de_get_len(hf_record) > sizeof(hf_record) || de_get_len(ag_record) > sizeof(ag_record) ||
        sdp_register_service(hf_record) != ERROR_CODE_SUCCESS || sdp_register_service(ag_record) != ERROR_CODE_SUCCESS) {
        say("[SDP] Service registration failed\n");
        return 3;
    }

    gap_set_local_name("Q7 Bridge");
    gap_set_class_of_device(0x200408); // audio, hands-free
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_set_bondable_mode(0);
    gap_discoverable_control(0);
    gap_connectable_control(1);
    gap_register_classic_connection_filter(connection_filter);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);

    static btstack_packet_callback_registration_t gap_registration;
    gap_registration.callback = gap_event;
    hci_add_event_handler(&gap_registration);
    hci_register_sco_packet_handler(sco_packet);

    btstack_stdin_setup(key);
    btstack_stdin_window_register_ctrl_c_callback(ctrl_c);
    btstack_run_loop_set_timer_handler(&tick_timer, tick);
    btstack_run_loop_set_timer(&tick_timer, 200);
    btstack_run_loop_add_timer(&tick_timer);

    char phone[20], headset[20];
    addr_or_none(phone, sizeof(phone), have_phone, phone_addr);
    addr_or_none(headset, sizeof(headset), have_headset, headset_addr);
    say("[APP] Saved phone: %s  Saved headset: %s\n", phone, headset);
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
