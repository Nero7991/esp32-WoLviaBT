#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Bluetooth Classic listener that answers for the host's BD_ADDR while the host is off.
// Runs the ESP32 BR/EDR controller over VHCI with no host stack: page scan only (not
// discoverable), and every HCI event is logged with a timestamp for measurement.

typedef enum {
    BT_WAKE_REJECT = 0, // reject the Connection Request at once (reason 0x0F)
    BT_WAKE_SILENT = 1, // do not answer; the controller rejects after Connection_Accept_Timeout
    BT_WAKE_ACCEPT = 2, // accept, authenticate with the host's link key, open the HID channels
                        // and hold the pad connected until the handover (needs a link key)
    BT_WAKE_PROBE = 3,  // measurement: on each Connection Request reset the controller (the
                        // half-open link vanishes, nothing is sent) and page scan again
} bt_wake_mode_t;

typedef enum {
    BT_SCAN_AUTO = 0, // page scan only while the host looks down
    BT_SCAN_ON = 1,
    BT_SCAN_OFF = 2,
} bt_scan_policy_t;

// How the board lets go of a held pad.
typedef enum {
    BT_DROP_DETACH = 0, // HCI_Disconnect, reason 0x13 (remote user terminated)
    BT_DROP_RESET = 1,  // HCI_Reset: the link vanishes without an LMP detach
    BT_DROP_RADIO = 2,  // esp_bt_controller_disable: radio off until re-enabled
    BT_DROP_L2CAP = 3,  // close the HID interrupt and control channels, keep the ACL link
} bt_drop_t;

typedef void (*bt_wake_press_cb_t)(void);

// host: the address to answer for (the host's adapter). Fixed until reboot.
// pad: only Connection Requests from this address count as a press.
// link_key: 16 bytes in BlueZ file order, or NULL (accept mode then falls back to reject).
esp_err_t bt_wake_start(const uint8_t host[6], const uint8_t pad[6], const uint8_t *link_key,
                        bt_wake_mode_t mode, bt_wake_press_cb_t cb);

void bt_wake_set_pad(const uint8_t pad[6]);
void bt_wake_set_link_key(const uint8_t key[16]);
void bt_wake_set_mode(bt_wake_mode_t mode);
void bt_wake_set_scan_policy(bt_scan_policy_t p);

// Automatic handover in accept mode: drop the held pad this many seconds after the host comes
// up (negative disables it), using this method.
void bt_wake_set_handover(int delay_s, bt_drop_t method);

// Let go of a held pad now. BT_DROP_RADIO leaves the radio off; bt_wake_radio_on restores it.
esp_err_t bt_wake_drop(bt_drop_t method);
// BT_DROP_DETACH with an explicit HCI_Disconnect reason (0x05, 0x13, 0x14, 0x15, 0x1A, 0x29, 0x3B).
esp_err_t bt_wake_detach(uint8_t reason);

// Role requested when accepting the pad: true = become central (role switch, as BlueZ does),
// false = stay peripheral so the pad remains central.
void bt_wake_set_accept_central(bool central);

// Accept-mode measurement: behave like the host up to a stage, then stop answering there.
// 0 none, 1 ignore the SDP channel request, 2 ignore SDP queries, 3 ignore the link key request,
// 4 negative link key reply, 5 ignore the HID interrupt channel request, 6 ignore configuration
// requests on the HID channels.
void bt_wake_set_cutoff(uint8_t stage);

// Reason used by reject mode (default 0x0F; 0x0D limited resources, 0x0E security reasons).
void bt_wake_set_reject_reason(uint8_t reason);
esp_err_t bt_wake_radio_on(void);

// Feed the host state (from the Ethernet ping check). A true -> false edge starts the
// post-shutdown ignore window, because a pad that was connected keeps paging after shutdown.
void bt_wake_set_host_up(bool up);

// Call once a second (timers for the automatic handover).
void bt_wake_tick(void);

void bt_wake_status_json(char *buf, size_t len);

// Learn mode, for finding a controller's address: page scan stays on and every Connection Request
// is recorded and rejected (never a press). It turns itself off when bt_wake_learned() has not
// been called for 20 s. A controller pages the host it is paired with, so this only sees pads
// paired with the host whose address this board answers for.
typedef struct {
    uint8_t addr[6];
    uint32_t cod;   // Class of Device from the request (DualSense: 0x002508, gamepad)
    uint16_t count; // requests seen
} bt_wake_seen_t;
void bt_wake_learn(bool on);
bool bt_wake_learning(void);
int bt_wake_learned(bt_wake_seen_t *out, int max);
// The address this board answers for (latched at bt_wake_start), false if not started.
bool bt_wake_host(uint8_t out[6]);
