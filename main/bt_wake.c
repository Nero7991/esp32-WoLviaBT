// bt_wake.c - answer for the host's BD_ADDR over BR/EDR and turn the pad's page into a press.
//
// A DualSense whose PS button is pressed while its host is off pages the host's BD_ADDR. With
// the ESP32 controller set to that address and page scanning, each page arrives here as an HCI
// Connection Request. Prior art for the reject path: github.com/lNikazzzl/esphome-gamepad-wake
// (MIT). The accept path adds just enough of a host (link key reply, L2CAP signalling for the two
// HID channels) to hold the pad connected while the real host boots.

#include "bt_wake.h"

#include <stdio.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bt_wake";

#define PRESS_COOLDOWN_MS     30000 // one press pages several times; one wake per burst
#define IGNORE_AFTER_DOWN_MS  60000 // a pad connected at shutdown keeps paging afterwards
#define BURST_GAP_MS          10000 // requests closer than this belong to one burst

#define ACCEPT_TIMEOUT_DEFAULT 0x1FA0 // 5 s (slots of 0.625 ms), the spec default
#define ACCEPT_TIMEOUT_SILENT  0xB540 // 29 s, the spec maximum

// HCI opcodes
#define OP_INQUIRY                0x0401
#define OP_INQUIRY_CANCEL         0x0402
#define OP_WRITE_INQUIRY_MODE     0x0C45
#define OP_ACCEPT_CONN_REQ        0x0409
#define OP_REJECT_CONN_REQ        0x040A
#define OP_LINK_KEY_REPLY         0x040B
#define OP_LINK_KEY_NEG_REPLY     0x040C
#define OP_PIN_NEG_REPLY          0x040E
#define OP_DISCONNECT             0x0406
#define OP_IO_CAP_NEG_REPLY       0x0434
#define OP_WRITE_DEF_LINK_POLICY  0x080F
#define OP_SET_EVENT_MASK         0x0C01
#define OP_RESET                  0x0C03
#define OP_WRITE_CONN_ACCEPT_TO   0x0C16
#define OP_WRITE_SCAN_ENABLE      0x0C1A
#define OP_WRITE_PAGE_SCAN_ACT    0x0C1C
#define OP_WRITE_PAGE_SCAN_TYPE   0x0C47
#define OP_WRITE_SSP_MODE         0x0C56
#define OP_READ_BD_ADDR           0x1009

// HCI events
#define EV_INQ_COMPLETE    0x01
#define EV_INQ_RESULT      0x02
#define EV_INQ_RESULT_RSSI 0x22
#define EV_EXT_INQ_RESULT  0x2F
#define EV_CONN_COMPLETE   0x03
#define EV_CONN_REQUEST    0x04
#define EV_DISCONN_COMPL   0x05
#define EV_AUTH_COMPLETE   0x06
#define EV_ENCRYPT_CHANGE  0x08
#define EV_CMD_COMPLETE    0x0E
#define EV_CMD_STATUS      0x0F
#define EV_NUM_COMPLETED   0x13
#define EV_PIN_REQUEST     0x16
#define EV_LINK_KEY_REQ    0x17
#define EV_IO_CAP_REQ      0x31
#define EV_IO_CAP_RSP      0x32
#define EV_USER_CONFIRM    0x33
#define EV_SSP_COMPLETE    0x36

// L2CAP signalling
#define L2_CMD_REJECT   0x01
#define L2_CONN_REQ     0x02
#define L2_CONN_RSP     0x03
#define L2_CONF_REQ     0x04
#define L2_CONF_RSP     0x05
#define L2_DISC_REQ     0x06
#define L2_DISC_RSP     0x07
#define L2_ECHO_REQ     0x08
#define L2_ECHO_RSP     0x09
#define L2_INFO_REQ     0x0A
#define L2_INFO_RSP     0x0B
#define PSM_HID_CTRL    0x0011
#define PSM_HID_INTR    0x0013
#define PSM_SDP         0x0001

typedef struct {
    int64_t t_us;
    uint16_t len;
    uint8_t data[96]; // HCI events and L2CAP signalling fit; HID reports are only counted
} hci_rx_t;

typedef struct {
    uint16_t psm, local, remote;
    bool cfg_in, cfg_out, open;
} l2_chan_t;

static QueueHandle_t s_rx_q;
static SemaphoreHandle_t s_send_ok;
static SemaphoreHandle_t s_cmd_done;
static SemaphoreHandle_t s_tx_lock;
static volatile bool s_init_phase;
static volatile uint16_t s_last_cmd_op;
static volatile uint8_t s_last_cmd_status;
static bool s_radio_on;

static uint8_t s_host[6], s_pad[6], s_bdaddr_read[6];
static uint8_t s_link_key[16];
static bool s_have_key;
static volatile bt_wake_mode_t s_mode;
static volatile bt_scan_policy_t s_policy = BT_SCAN_AUTO;
static volatile bool s_host_up;
static volatile int64_t s_host_down_at_ms = -1;
static bool s_scanning;
static bt_wake_press_cb_t s_cb;
static bool s_started;

#define LEARN_MAX 6
#define LEARN_IDLE_MS 20000
static volatile bool s_learn;
static volatile int64_t s_learn_poll_ms;
static bt_wake_seen_t s_seen[LEARN_MAX];
static int s_nseen;
static portMUX_TYPE s_seen_lock = portMUX_INITIALIZER_UNLOCKED;

static int s_handover_delay_s = -1;
static bt_drop_t s_handover_method = BT_DROP_RESET;
static volatile int64_t s_handover_at_ms = -1;
static bool s_accept_central = true;
static uint8_t s_reject_reason = 0x0F;
static uint8_t s_cutoff;
static int64_t s_drop_at_ms = -1;

// Held connection
static volatile uint16_t s_conn = 0xFFFF;
static int64_t s_conn_at_ms = -1, s_first_report_ms = -1;
static uint32_t s_hid_reports;
static l2_chan_t s_ch[3]; // 0 HID control, 1 HID interrupt, 2 SDP
static uint8_t s_sig_id = 1;
static int64_t s_last_hold_ms = -1;
static uint8_t s_last_disc_reason;

// Measurement counters
static uint32_t s_requests, s_pad_requests, s_presses, s_ignored;
static int64_t s_last_press_ms = -1000000;
static int64_t s_burst_first_ms = -1, s_burst_last_ms = -1;
static uint32_t s_burst_count, s_last_burst_count;
static int64_t s_last_burst_span_ms = -1;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void fmt_addr(const uint8_t a[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

// HCI carries BD_ADDR little-endian; we keep addresses in display order.
static void addr_from_hci(const uint8_t *p, uint8_t out[6])
{
    for (int i = 0; i < 6; i++) {
        out[i] = p[5 - i];
    }
}

static void addr_to_hci(const uint8_t a[6], uint8_t *p)
{
    for (int i = 0; i < 6; i++) {
        p[i] = a[5 - i];
    }
}

static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }

// ---- VHCI plumbing ----

static void vhci_send_available(void)
{
    xSemaphoreGive(s_send_ok);
}

static int vhci_recv(uint8_t *data, uint16_t len)
{
    hci_rx_t m = { .t_us = esp_timer_get_time(), .len = len };
    memcpy(m.data, data, len < sizeof(m.data) ? len : sizeof(m.data));
    if (s_init_phase && len >= 6 && data[0] == 0x04 &&
        (data[1] == EV_CMD_COMPLETE || data[1] == EV_CMD_STATUS)) {
        if (data[1] == EV_CMD_COMPLETE) {
            s_last_cmd_op = data[4] | (data[5] << 8);
            s_last_cmd_status = len > 6 ? data[6] : 0xFF;
            if (s_last_cmd_op == OP_READ_BD_ADDR && len >= 13) {
                addr_from_hci(&data[7], s_bdaddr_read);
            }
        } else {
            s_last_cmd_status = data[3];
            s_last_cmd_op = data[5] | (data[6] << 8);
        }
        xSemaphoreGive(s_cmd_done);
    }
    xQueueSend(s_rx_q, &m, 0); // a full queue only loses log lines
    return 0;
}

static const esp_vhci_host_callback_t s_vhci_cb = {
    .notify_host_send_available = vhci_send_available,
    .notify_host_recv = vhci_recv,
};

static void send_raw(uint8_t *pkt, uint16_t len)
{
    while (!esp_vhci_host_check_send_available()) {
        xSemaphoreTake(s_send_ok, pdMS_TO_TICKS(100));
    }
    esp_vhci_host_send_packet(pkt, len);
}

static esp_err_t hci_cmd(uint16_t op, const uint8_t *params, uint8_t plen, bool wait)
{
    uint8_t pkt[4 + 32];
    if (plen > 32 || !s_radio_on) {
        return ESP_ERR_INVALID_STATE;
    }
    pkt[0] = 0x01;
    put16(&pkt[1], op);
    pkt[3] = plen;
    if (plen) {
        memcpy(&pkt[4], params, plen);
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (wait) {
        xSemaphoreTake(s_cmd_done, 0);
    }
    send_raw(pkt, 4 + plen);
    esp_err_t ret = ESP_OK;
    if (wait) {
        if (xSemaphoreTake(s_cmd_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(TAG, "HCI 0x%04X: no Command Complete", op);
            ret = ESP_ERR_TIMEOUT;
        } else if (s_last_cmd_op != op || s_last_cmd_status != 0) {
            ESP_LOGE(TAG, "HCI 0x%04X: status 0x%02X (op 0x%04X)", op, s_last_cmd_status,
                     s_last_cmd_op);
            ret = ESP_FAIL;
        }
    }
    xSemaphoreGive(s_tx_lock);
    return ret;
}

// One L2CAP frame on the held connection (fits a single ACL packet).
static void l2_send(uint16_t cid, const uint8_t *payload, uint16_t plen)
{
    uint8_t pkt[5 + 4 + 48];
    if (s_conn == 0xFFFF || plen > 48 || !s_radio_on) {
        return;
    }
    pkt[0] = 0x02;
    put16(&pkt[1], (s_conn & 0x0FFF) | (0x2 << 12)); // PB=10: first automatically-flushable
    put16(&pkt[3], 4 + plen);
    put16(&pkt[5], plen);
    put16(&pkt[7], cid);
    memcpy(&pkt[9], payload, plen);
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    send_raw(pkt, 9 + plen);
    xSemaphoreGive(s_tx_lock);
}

static void sig_send(uint8_t code, uint8_t id, const uint8_t *data, uint16_t dlen)
{
    uint8_t p[4 + 40];
    p[0] = code;
    p[1] = id;
    put16(&p[2], dlen);
    if (dlen) {
        memcpy(&p[4], data, dlen);
    }
    l2_send(0x0001, p, 4 + dlen);
}

static esp_err_t controller_setup(void)
{
    s_init_phase = true;
    esp_err_t e = hci_cmd(OP_RESET, NULL, 0, true);
    // BlueZ's BR/EDR event mask: adds the SSP and link-key events to the default.
    static const uint8_t mask[8] = { 0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D };
    if (e == ESP_OK) e = hci_cmd(OP_SET_EVENT_MASK, mask, 8, true);
    uint8_t ssp = 1; // the stored key came from Secure Simple Pairing (BlueZ key type 4)
    if (e == ESP_OK) e = hci_cmd(OP_WRITE_SSP_MODE, &ssp, 1, true);
    uint8_t pol[2] = { 0x05, 0x00 }; // allow role switch and sniff
    if (e == ESP_OK) e = hci_cmd(OP_WRITE_DEF_LINK_POLICY, pol, 2, true);
    uint8_t act[4] = { 0x00, 0x04, 0x24, 0x00 }; // interval 0x0400 (640 ms), window 0x0024
    if (e == ESP_OK) e = hci_cmd(OP_WRITE_PAGE_SCAN_ACT, act, 4, true);
    uint8_t type = 0x01; // interlaced
    if (e == ESP_OK) e = hci_cmd(OP_WRITE_PAGE_SCAN_TYPE, &type, 1, true);
    uint16_t to = s_mode == BT_WAKE_SILENT ? ACCEPT_TIMEOUT_SILENT : ACCEPT_TIMEOUT_DEFAULT;
    uint8_t top[2] = { to & 0xFF, to >> 8 };
    if (e == ESP_OK) e = hci_cmd(OP_WRITE_CONN_ACCEPT_TO, top, 2, true);
    uint8_t inq_mode = 0x02; // inquiry results with RSSI or extended data (carries the name)
    if (e == ESP_OK) e = hci_cmd(OP_WRITE_INQUIRY_MODE, &inq_mode, 1, true);
    if (e == ESP_OK) e = hci_cmd(OP_READ_BD_ADDR, NULL, 0, true);
    s_init_phase = false;
    s_scanning = false; // reset turned it off
    return e;
}

static void apply_scan(void)
{
    if (!s_radio_on) {
        return;
    }
    bool want = s_learn || s_policy == BT_SCAN_ON || (s_policy == BT_SCAN_AUTO && !s_host_up);
    if (want == s_scanning) {
        return;
    }
    uint8_t en = want ? 0x02 : 0x00; // 0x02: page scan only, never inquiry scan
    hci_cmd(OP_WRITE_SCAN_ENABLE, &en, 1, false);
    s_scanning = want;
    ESP_LOGI(TAG, "page scan %s (policy %s, host %s)", want ? "ON" : "OFF",
             s_policy == BT_SCAN_AUTO ? "auto" : s_policy == BT_SCAN_ON ? "on" : "off",
             s_host_up ? "up" : "down");
}

static void conn_clear(const char *why)
{
    if (s_conn != 0xFFFF) {
        s_last_hold_ms = now_ms() - s_conn_at_ms;
        ESP_LOGI(TAG, "HOLD END t=%lld held %lld ms, %lu HID reports (%s)", now_ms(),
                 s_last_hold_ms, (unsigned long)s_hid_reports, why);
    }
    s_conn = 0xFFFF;
    memset(s_ch, 0, sizeof(s_ch));
    s_handover_at_ms = -1;
}

// ---- events ----

static bool accepting(void)
{
    return s_mode == BT_WAKE_ACCEPT && s_have_key && s_conn == 0xFFFF;
}

// Record a device seen in learn mode. name may be NULL.
static void learn_add(const uint8_t addr[6], uint32_t cod, bool paged, int8_t rssi,
                      const char *name)
{
    taskENTER_CRITICAL(&s_seen_lock);
    int i = 0;
    while (i < s_nseen && memcmp(s_seen[i].addr, addr, 6) != 0) {
        i++;
    }
    if (i == s_nseen && s_nseen < LEARN_MAX) {
        memset(&s_seen[i], 0, sizeof(s_seen[i]));
        memcpy(s_seen[i].addr, addr, 6);
        s_nseen++;
    }
    if (i < s_nseen) {
        bt_wake_seen_t *e = &s_seen[i];
        e->cod = cod;
        e->count++;
        if (paged) {
            e->paged = true;
        } else {
            e->inquiry = true;
            if (rssi) {
                e->rssi = rssi;
            }
            if (name && name[0]) {
                snprintf(e->name, sizeof(e->name), "%s", name);
            }
        }
    }
    taskEXIT_CRITICAL(&s_seen_lock);
}

// Inquiry: General Inquiry Access Code, 6 x 1.28 s, unlimited responses. Restarted on completion
// while learning.
static void inquiry_start(void)
{
    static const uint8_t p[5] = { 0x33, 0x8B, 0x9E, 0x06, 0x00 };
    hci_cmd(OP_INQUIRY, p, 5, false);
}

// Extended inquiry data: [len][type][data...]; 0x09 complete name, 0x08 shortened name.
static void eir_name(const uint8_t *eir, int n, char *out, size_t olen)
{
    out[0] = '\0';
    for (int i = 0; i + 1 < n && eir[i]; i += eir[i] + 1) {
        int l = eir[i], t = eir[i + 1];
        if ((t == 0x09 || t == 0x08) && l >= 2) {
            int nl = l - 1;
            if (i + 2 + nl > n) {
                nl = n - i - 2; // truncated by our receive buffer
            }
            if (nl <= 0) {
                return;
            }
            size_t k = 0;
            for (int j = 0; j < nl && k + 1 < olen; j++) {
                uint8_t c = eir[i + 2 + j];
                out[k++] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? (char)c : '?';
            }
            out[k] = '\0';
            if (t == 0x09) {
                return;
            }
        }
    }
}

static void on_inquiry_result(const hci_rx_t *m)
{
    const uint8_t *d = m->data;
    int n = m->len < sizeof(m->data) ? m->len : (int)sizeof(m->data);
    uint8_t ev = d[1], addr[6];
    if (n < 16 || d[3] < 1) {
        return;
    }
    addr_from_hci(&d[4], addr); // first response only (controllers send one per event)
    uint32_t cod;
    int8_t rssi = 0;
    char name[32] = "";
    if (ev == EV_INQ_RESULT) { // addr, psrm, reserved 2, cod, clock offset
        cod = d[13] | (d[14] << 8) | (d[15] << 16);
    } else {                   // addr, psrm, reserved 1, cod, clock offset, rssi [, eir]
        if (n < 18) {
            return;
        }
        cod = d[12] | (d[13] << 8) | (d[14] << 16);
        rssi = (int8_t)d[17];
        if (ev == EV_EXT_INQ_RESULT && n > 18) {
            eir_name(&d[18], n - 18, name, sizeof(name));
        }
    }
    char a[18];
    fmt_addr(addr, a);
    ESP_LOGI(TAG, "INQUIRY %s cod=0x%06lX rssi %d name \"%s\"", a, (unsigned long)cod, rssi, name);
    learn_add(addr, cod, false, rssi, name);
}

static void on_conn_request(const hci_rx_t *m)
{
    const uint8_t *p = &m->data[3];
    uint8_t addr[6];
    addr_from_hci(p, addr);
    uint32_t cod = p[6] | (p[7] << 8) | (p[8] << 16);
    uint8_t link = p[9];
    int64_t t = m->t_us / 1000;
    char a[18];
    fmt_addr(addr, a);
    s_requests++;

    if (s_learn) {
        learn_add(addr, cod, true, 0, NULL);
        ESP_LOGI(TAG, "LEARN t=%lld ms %s cod=0x%06lX", t, a, (unsigned long)cod);
        uint8_t rj[7];
        addr_to_hci(addr, rj);
        rj[6] = 0x0F;
        hci_cmd(OP_REJECT_CONN_REQ, rj, 7, false);
        return;
    }

    bool is_pad = memcmp(addr, s_pad, 6) == 0;
    if (is_pad) {
        s_pad_requests++;
        if (s_burst_last_ms < 0 || t - s_burst_last_ms > BURST_GAP_MS) {
            if (s_burst_count) {
                s_last_burst_count = s_burst_count;
                s_last_burst_span_ms = s_burst_last_ms - s_burst_first_ms;
            }
            s_burst_first_ms = t;
            s_burst_count = 0;
        }
        s_burst_count++;
        s_burst_last_ms = t;
    }

    ESP_LOGI(TAG, "CONN_REQ t=%lld ms from %s cod=0x%06lX link=%u%s burst#%lu +%lld ms", t, a,
             (unsigned long)cod, link, is_pad ? " PAD" : "", (unsigned long)s_burst_count,
             is_pad ? t - s_burst_first_ms : 0);
    if (is_pad && s_drop_at_ms >= 0) {
        ESP_LOGI(TAG, "REPAGE after drop: +%lld ms", t - s_drop_at_ms);
    }

    uint8_t r[7];
    addr_to_hci(addr, r);
    bool probe_reset = false;
    if (s_mode == BT_WAKE_PROBE) {
        probe_reset = true; // after the press bookkeeping below
    } else if (is_pad && link == 1 && accepting()) {
        r[6] = s_accept_central ? 0x00 : 0x01; // 0x00 role switch to central (as BlueZ)
        hci_cmd(OP_ACCEPT_CONN_REQ, r, 7, false);
        ESP_LOGI(TAG, "accepting %s", a);
    } else if (s_mode != BT_WAKE_SILENT) {
        r[6] = s_reject_reason; // default 0x0F: Connection Rejected due to Unacceptable BD_ADDR
        hci_cmd(OP_REJECT_CONN_REQ, r, 7, false);
        ESP_LOGI(TAG, "rejected with reason 0x%02X", s_reject_reason);
    }

    if (probe_reset) {
        if (s_drop_at_ms < 0 || !is_pad) {
            s_drop_at_ms = t; // first page of this probe: REPAGE times are relative to it
        }
        controller_setup();
        apply_scan();
        ESP_LOGI(TAG, "PROBE reset done t=%lld (+%lld ms)", now_ms(), now_ms() - t);
    }
    if (!is_pad) {
        return;
    }
    if (s_host_down_at_ms >= 0 && t - s_host_down_at_ms < IGNORE_AFTER_DOWN_MS) {
        s_ignored++;
        ESP_LOGI(TAG, "press ignored: %lld ms after host went down", t - s_host_down_at_ms);
        return;
    }
    if (t - s_last_press_ms < PRESS_COOLDOWN_MS) {
        return;
    }
    s_last_press_ms = t;
    s_presses++;
    ESP_LOGI(TAG, "PRESS #%lu", (unsigned long)s_presses);
    if (s_cb) {
        s_cb();
    }
}

static l2_chan_t *chan_by_local(uint16_t cid)
{
    for (int i = 0; i < 3; i++) {
        if (s_ch[i].local == cid) {
            return &s_ch[i];
        }
    }
    return NULL;
}

static void chan_check_open(l2_chan_t *c, int64_t t)
{
    if (!c->open && c->cfg_in && c->cfg_out) {
        c->open = true;
        ESP_LOGI(TAG, "L2CAP %s open t=%lld (+%lld ms from connect), local 0x%04X remote 0x%04X",
                 c->psm == PSM_HID_CTRL ? "HID control" : c->psm == PSM_HID_INTR ? "HID interrupt"
                                                                                  : "SDP",
                 t, t - s_conn_at_ms,
                 c->local, c->remote);
    }
}

static void on_signalling(const uint8_t *p, uint16_t len, int64_t t)
{
    while (len >= 4) {
        uint8_t code = p[0], id = p[1];
        uint16_t dlen = le16(&p[2]);
        const uint8_t *d = &p[4];
        if (4 + dlen > len) {
            break;
        }
        uint8_t out[16];
        switch (code) {
        case L2_CONN_REQ: {
            uint16_t psm = le16(d), scid = le16(&d[2]);
            int slot = psm == PSM_HID_CTRL ? 0 : psm == PSM_HID_INTR ? 1 : psm == PSM_SDP ? 2 : -1;
            ESP_LOGI(TAG, "L2CAP conn req t=%lld psm 0x%04X scid 0x%04X", t, psm, scid);
            if ((s_cutoff == 1 && psm == PSM_SDP) || (s_cutoff == 5 && psm == PSM_HID_INTR)) {
                ESP_LOGI(TAG, "CUTOFF %u: not answering conn req for psm 0x%04X", s_cutoff, psm);
                break;
            }
            put16(&out[2], scid);
            put16(&out[6], 0); // status
            if (slot < 0) {
                put16(&out[0], 0);
                put16(&out[4], 0x0002); // PSM not supported
                sig_send(L2_CONN_RSP, id, out, 8);
                break;
            }
            l2_chan_t *c = &s_ch[slot];
            memset(c, 0, sizeof(*c));
            c->psm = psm;
            c->local = 0x0040 + slot;
            c->remote = scid;
            put16(&out[0], c->local);
            put16(&out[4], 0); // success
            sig_send(L2_CONN_RSP, id, out, 8);
            // Our side of the configuration: no options (defaults, MTU 672).
            put16(&out[0], c->remote);
            put16(&out[2], 0);
            sig_send(L2_CONF_REQ, s_sig_id++, out, 4);
            break;
        }
        case L2_CONF_REQ: {
            l2_chan_t *c = chan_by_local(le16(d));
            if (!c) {
                break;
            }
            if (s_cutoff == 6 && c->psm != PSM_SDP) {
                ESP_LOGI(TAG, "CUTOFF 6: not answering config req t=%lld on psm 0x%04X", t, c->psm);
                break;
            }
            put16(&out[0], c->remote);
            put16(&out[2], 0); // flags
            put16(&out[4], 0); // success, options accepted as sent
            sig_send(L2_CONF_RSP, id, out, 6);
            c->cfg_in = true;
            chan_check_open(c, t);
            break;
        }
        case L2_CONF_RSP: {
            l2_chan_t *c = chan_by_local(le16(d));
            uint16_t result = dlen >= 6 ? le16(&d[4]) : 0xFFFF;
            if (c && result == 0) {
                c->cfg_out = true;
                chan_check_open(c, t);
            } else {
                ESP_LOGW(TAG, "L2CAP config rsp result 0x%04X", result);
            }
            break;
        }
        case L2_DISC_REQ: {
            l2_chan_t *c = chan_by_local(le16(d));
            ESP_LOGI(TAG, "L2CAP disconnect req t=%lld for 0x%04X", t, le16(d));
            memcpy(out, d, 4);
            sig_send(L2_DISC_RSP, id, out, 4);
            if (c) {
                memset(c, 0, sizeof(*c));
            }
            break;
        }
        case L2_DISC_RSP:
            ESP_LOGI(TAG, "L2CAP disconnect rsp t=%lld dcid 0x%04X scid 0x%04X", t, le16(d),
                     dlen >= 4 ? le16(&d[2]) : 0);
            break;
        case L2_ECHO_REQ:
            sig_send(L2_ECHO_RSP, id, NULL, 0);
            break;
        case L2_INFO_REQ:
            put16(&out[0], dlen >= 2 ? le16(d) : 0);
            put16(&out[2], 0x0001); // not supported
            sig_send(L2_INFO_RSP, id, out, 4);
            break;
        case L2_CMD_REJECT:
            ESP_LOGW(TAG, "L2CAP command reject t=%lld reason 0x%04X", t, dlen >= 2 ? le16(d) : 0);
            break;
        default:
            ESP_LOGI(TAG, "L2CAP signalling code 0x%02X t=%lld", code, t);
        }
        p += 4 + dlen;
        len -= 4 + dlen;
    }
}

// The pad queries the host's SDP records once per connection (seen with btmon against BlueZ:
// one ServiceSearchAttributeRequest for UUID L2CAP, attributes 0x0000-0xFFFF) and closes the
// channel before it authenticates. Answer every query with an empty record list.
static void on_sdp(const uint8_t *p, uint16_t len, int64_t t)
{
    if (len < 5) {
        return;
    }
    uint8_t pdu = p[0];
    char hex[3 * 24 + 1];
    int n = 0;
    for (int i = 0; i < len && i < 24; i++) {
        n += snprintf(hex + n, sizeof(hex) - n, "%02X ", p[i]);
    }
    ESP_LOGI(TAG, "SDP request t=%lld pdu 0x%02X: %s", t, pdu, hex);
    if (s_cutoff == 2) {
        ESP_LOGI(TAG, "CUTOFF 2: not answering SDP");
        return;
    }
    uint8_t r[16];
    r[1] = p[1]; // transaction id
    r[2] = p[2];
    uint16_t plen;
    switch (pdu) {
    case 0x06: // ServiceSearchAttributeRequest -> empty AttributeLists
    case 0x04: // ServiceAttributeRequest -> empty AttributeList
        r[0] = pdu + 1;
        r[5] = 0x00; r[6] = 0x02;      // AttributeListsByteCount = 2 (SDP is big-endian)
        r[7] = 0x35; r[8] = 0x00;      // data element sequence, length 0
        r[9] = 0x00;                   // no continuation
        plen = 5;
        break;
    case 0x02: // ServiceSearchRequest -> no handles
        r[0] = 0x03;
        r[5] = 0; r[6] = 0;            // total
        r[7] = 0; r[8] = 0;            // current
        r[9] = 0x00;
        plen = 5;
        break;
    default:
        r[0] = 0x01;                   // ErrorResponse: invalid request syntax
        r[5] = 0x00; r[6] = 0x03;
        plen = 2;
    }
    r[3] = plen >> 8;
    r[4] = plen & 0xFF;
    l2_send(s_ch[2].remote, r, 5 + plen);
}

static void on_acl(const hci_rx_t *m)
{
    const uint8_t *d = m->data;
    int64_t t = m->t_us / 1000;
    uint16_t hf = le16(&d[1]);
    uint8_t pb = (hf >> 12) & 0x3;
    if (m->len < 9 || pb == 0x1) {
        return; // continuation fragment: only HID reports are long enough to need them
    }
    uint16_t l2len = le16(&d[5]), cid = le16(&d[7]);
    const uint8_t *pl = &d[9];
    uint16_t avail = (m->len < sizeof(m->data) ? m->len : sizeof(m->data)) - 9;
    if (cid == 0x0001) {
        on_signalling(pl, l2len < avail ? l2len : avail, t);
        return;
    }
    if (s_ch[2].open && cid == s_ch[2].local) {
        on_sdp(pl, l2len < avail ? l2len : avail, t);
        return;
    }
    if (s_ch[1].open && cid == s_ch[1].local) {
        if (s_hid_reports++ == 0) {
            s_first_report_ms = t;
            ESP_LOGI(TAG, "first HID report t=%lld (+%lld ms from connect) %02X %02X len %u", t,
                     t - s_conn_at_ms, pl[0], pl[1], l2len);
        }
        return;
    }
    ESP_LOGI(TAG, "ACL cid 0x%04X len %u t=%lld", cid, l2len, t);
}

static void on_event(const hci_rx_t *m)
{
    const uint8_t *d = m->data;
    int64_t t = m->t_us / 1000;
    uint8_t ev = d[1];
    uint8_t a6[6], r[22];
    char a[18];
    switch (ev) {
    case EV_INQ_RESULT:
    case EV_INQ_RESULT_RSSI:
    case EV_EXT_INQ_RESULT:
        if (s_learn) {
            on_inquiry_result(m);
        }
        return;
    case EV_INQ_COMPLETE:
        if (s_learn) {
            inquiry_start();
        }
        return;
    case EV_CONN_REQUEST:
        if (m->len >= 13) {
            on_conn_request(m);
        }
        return;
    case EV_CMD_COMPLETE:
        return;
    case EV_NUM_COMPLETED:
        return;
    case EV_CMD_STATUS:
        if (d[3] != 0) {
            ESP_LOGW(TAG, "CMD_STATUS t=%lld op 0x%04X status 0x%02X", t, le16(&d[5]), d[3]);
        }
        return;
    case EV_CONN_COMPLETE:
        addr_from_hci(&d[6], a6);
        fmt_addr(a6, a);
        ESP_LOGI(TAG, "CONN_COMPLETE t=%lld status 0x%02X handle 0x%04X %s", t, d[3], le16(&d[4]),
                 a);
        if (d[3] == 0 && memcmp(a6, s_pad, 6) == 0) {
            s_conn = le16(&d[4]) & 0x0FFF;
            s_conn_at_ms = t;
            s_hid_reports = 0;
            s_first_report_ms = -1;
            memset(s_ch, 0, sizeof(s_ch));
            ESP_LOGI(TAG, "HOLD START t=%lld", t);
            if (s_host_up && s_handover_delay_s >= 0) {
                s_handover_at_ms = t + s_handover_delay_s * 1000LL;
            }
        }
        return;
    case EV_DISCONN_COMPL:
        s_last_disc_reason = d[6];
        ESP_LOGI(TAG, "DISCONN_COMPLETE t=%lld status 0x%02X handle 0x%04X reason 0x%02X", t,
                 d[3], le16(&d[4]), d[6]);
        if ((le16(&d[4]) & 0x0FFF) == s_conn) {
            conn_clear("disconnect event");
        }
        return;
    case EV_LINK_KEY_REQ:
        addr_from_hci(&d[3], a6);
        fmt_addr(a6, a);
        memcpy(r, &d[3], 6);
        if (s_cutoff == 3) {
            ESP_LOGI(TAG, "LINK_KEY_REQ t=%lld from %s: CUTOFF 3, no reply", t, a);
        } else if (s_cutoff == 4) {
            hci_cmd(OP_LINK_KEY_NEG_REPLY, r, 6, false);
            ESP_LOGI(TAG, "LINK_KEY_REQ t=%lld from %s: CUTOFF 4, negative reply", t, a);
        } else if (s_have_key && memcmp(a6, s_pad, 6) == 0) {
            memcpy(&r[6], s_link_key, 16); // never logged
            hci_cmd(OP_LINK_KEY_REPLY, r, 22, false);
            ESP_LOGI(TAG, "LINK_KEY_REQ t=%lld from %s: replied with stored key", t, a);
        } else {
            hci_cmd(OP_LINK_KEY_NEG_REPLY, r, 6, false);
            ESP_LOGI(TAG, "LINK_KEY_REQ t=%lld from %s: no key, negative reply", t, a);
        }
        return;
    case EV_PIN_REQUEST:
        memcpy(r, &d[3], 6);
        hci_cmd(OP_PIN_NEG_REPLY, r, 6, false);
        ESP_LOGW(TAG, "PIN request t=%lld: refused", t);
        return;
    case EV_IO_CAP_REQ:
        memcpy(r, &d[3], 6);
        r[6] = 0x18; // pairing not allowed: we only reuse the host's existing key
        hci_cmd(OP_IO_CAP_NEG_REPLY, r, 7, false);
        ESP_LOGW(TAG, "IO capability request t=%lld (pairing attempt): refused", t);
        return;
    case EV_IO_CAP_RSP:
        ESP_LOGI(TAG, "IO_CAP_RESPONSE t=%lld io %u oob %u auth 0x%02X (pad wants to pair)", t,
                 d[9], d[10], d[11]);
        return;
    case EV_SSP_COMPLETE:
        ESP_LOGI(TAG, "SSP_COMPLETE t=%lld status 0x%02X", t, d[3]);
        return;
    case EV_AUTH_COMPLETE:
        ESP_LOGI(TAG, "AUTH_COMPLETE t=%lld status 0x%02X", t, d[3]);
        return;
    case EV_ENCRYPT_CHANGE:
        ESP_LOGI(TAG, "ENCRYPTION t=%lld status 0x%02X enabled %u (+%lld ms from connect)", t,
                 d[3], d[6], s_conn_at_ms >= 0 ? t - s_conn_at_ms : -1);
        return;
    default: {
        char hex[3 * 20 + 1];
        int n = 0, max = m->len < 20 ? m->len : 20;
        for (int i = 0; i < max; i++) {
            n += snprintf(hex + n, sizeof(hex) - n, "%02X ", d[i]);
        }
        ESP_LOGI(TAG, "EVENT 0x%02X t=%lld len %u: %s", ev, t, m->len, hex);
    }
    }
}

static void rx_task(void *arg)
{
    hci_rx_t m;
    for (;;) {
        if (xQueueReceive(s_rx_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (m.len >= 3 && m.data[0] == 0x04) {
            on_event(&m);
        } else if (m.data[0] == 0x02) {
            on_acl(&m);
        }
    }
}

// ---- public ----

esp_err_t bt_wake_start(const uint8_t host[6], const uint8_t pad[6], const uint8_t *link_key,
                        bt_wake_mode_t mode, bt_wake_press_cb_t cb)
{
    memcpy(s_host, host, 6);
    memcpy(s_pad, pad, 6);
    if (link_key) {
        memcpy(s_link_key, link_key, 16);
        s_have_key = true;
    }
    s_mode = mode;
    s_cb = cb;

    s_rx_q = xQueueCreate(24, sizeof(hci_rx_t));
    s_send_ok = xSemaphoreCreateBinary();
    s_cmd_done = xSemaphoreCreateBinary();
    s_tx_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rx_q && s_send_ok && s_cmd_done && s_tx_lock, ESP_ERR_NO_MEM, TAG,
                        "alloc");

    // The BD_ADDR is latched at controller init.
    ESP_RETURN_ON_ERROR(esp_iface_mac_addr_set(host, ESP_MAC_BT), TAG, "set BT address");
    ESP_RETURN_ON_ERROR(esp_bt_controller_mem_release(ESP_BT_MODE_BLE), TAG, "release BLE");
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&cfg), TAG, "controller init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), TAG, "enable");
    ESP_RETURN_ON_ERROR(esp_vhci_host_register_callback(&s_vhci_cb), TAG, "vhci cb");
    s_radio_on = true;

    if (xTaskCreate(rx_task, "bt_wake", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(controller_setup(), TAG, "controller setup");

    char want[18], got[18], pd[18];
    fmt_addr(host, want);
    fmt_addr(s_bdaddr_read, got);
    fmt_addr(pad, pd);
    ESP_LOGI(TAG, "controller up: BD_ADDR %s (wanted %s), pad %s, mode %d, link key %s", got,
             want, pd, (int)mode, s_have_key ? "set" : "none");
    if (memcmp(host, s_bdaddr_read, 6) != 0) {
        ESP_LOGE(TAG, "BD_ADDR override did not take effect");
    }
    s_started = true;
    apply_scan();
    return ESP_OK;
}

void bt_wake_set_pad(const uint8_t pad[6])
{
    memcpy(s_pad, pad, 6);
}

void bt_wake_set_link_key(const uint8_t key[16])
{
    memcpy(s_link_key, key, 16);
    s_have_key = true;
}

void bt_wake_set_mode(bt_wake_mode_t mode)
{
    s_mode = mode;
    if (mode == BT_WAKE_PROBE) {
        s_drop_at_ms = -1; // the next page starts a new probe timeline
    }
    if (s_tx_lock) {
        uint16_t to = mode == BT_WAKE_SILENT ? ACCEPT_TIMEOUT_SILENT : ACCEPT_TIMEOUT_DEFAULT;
        uint8_t p[2] = { to & 0xFF, to >> 8 };
        hci_cmd(OP_WRITE_CONN_ACCEPT_TO, p, 2, false);
    }
}

void bt_wake_set_scan_policy(bt_scan_policy_t p)
{
    s_policy = p;
    if (s_tx_lock) {
        apply_scan();
    }
}

void bt_wake_set_handover(int delay_s, bt_drop_t method)
{
    s_handover_delay_s = delay_s;
    s_handover_method = method;
}

void bt_wake_set_cutoff(uint8_t stage)
{
    s_cutoff = stage;
    ESP_LOGI(TAG, "cutoff stage %u", stage);
}

void bt_wake_set_reject_reason(uint8_t reason)
{
    s_reject_reason = reason;
}

void bt_wake_set_accept_central(bool central)
{
    s_accept_central = central;
}

esp_err_t bt_wake_detach(uint8_t reason)
{
    ESP_LOGI(TAG, "DROP t=%lld method detach reason 0x%02X, holding %s", now_ms(), reason,
             s_conn != 0xFFFF ? "yes" : "no");
    if (s_conn == 0xFFFF) {
        return ESP_ERR_INVALID_STATE;
    }
    s_drop_at_ms = now_ms();
    uint8_t p[3];
    put16(p, s_conn);
    p[2] = reason;
    return hci_cmd(OP_DISCONNECT, p, 3, false);
}

esp_err_t bt_wake_drop(bt_drop_t method)
{
    static const char *names[] = { "detach", "reset", "radio", "l2cap" };
    s_drop_at_ms = now_ms();
    ESP_LOGI(TAG, "DROP t=%lld method %s, holding %s", now_ms(), names[method],
             s_conn != 0xFFFF ? "yes" : "no");
    switch (method) {
    case BT_DROP_DETACH: {
        if (s_conn == 0xFFFF) {
            return ESP_ERR_INVALID_STATE;
        }
        uint8_t p[3];
        put16(p, s_conn);
        p[2] = 0x13; // remote user terminated connection
        return hci_cmd(OP_DISCONNECT, p, 3, false);
    }
    case BT_DROP_RESET: {
        conn_clear("controller reset");
        esp_err_t e = controller_setup();
        apply_scan();
        return e;
    }
    case BT_DROP_RADIO:
        conn_clear("radio off");
        s_radio_on = false;
        s_scanning = false;
        return esp_bt_controller_disable();
    case BT_DROP_L2CAP: {
        if (s_conn == 0xFFFF) {
            return ESP_ERR_INVALID_STATE;
        }
        // Interrupt first, then control: the order HID hosts use (HID spec 7.4.4).
        for (int i = 1; i >= 0; i--) {
            if (s_ch[i].open) {
                uint8_t d[4];
                put16(&d[0], s_ch[i].remote); // destination CID (the pad's)
                put16(&d[2], s_ch[i].local);  // source CID (ours)
                sig_send(L2_DISC_REQ, s_sig_id++, d, 4);
                s_ch[i].open = false;
            }
        }
        return ESP_OK;
    }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t bt_wake_radio_on(void)
{
    if (s_radio_on) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), TAG, "enable");
    s_radio_on = true;
    esp_err_t e = controller_setup();
    apply_scan();
    ESP_LOGI(TAG, "radio on t=%lld", now_ms());
    return e;
}

void bt_wake_set_host_up(bool up)
{
    if (s_host_up && !up) {
        s_host_down_at_ms = now_ms();
    }
    if (s_host_up != up) {
        ESP_LOGI(TAG, "host %s t=%lld", up ? "up" : "down", now_ms());
        if (up && s_conn != 0xFFFF && s_handover_delay_s >= 0) {
            s_handover_at_ms = now_ms() + s_handover_delay_s * 1000LL;
            ESP_LOGI(TAG, "handover in %d s", s_handover_delay_s);
        }
    }
    s_host_up = up;
    if (s_tx_lock) {
        apply_scan();
    }
}

void bt_wake_learn(bool on)
{
    if (!s_started) {
        return;
    }
    if (on) {
        taskENTER_CRITICAL(&s_seen_lock);
        s_nseen = 0;
        taskEXIT_CRITICAL(&s_seen_lock);
        s_learn_poll_ms = now_ms();
    }
    bool was = s_learn;
    s_learn = on;
    if (on != was) {
        ESP_LOGI(TAG, "learn mode %s", on ? "ON" : "OFF");
        if (on) {
            inquiry_start();
        } else {
            hci_cmd(OP_INQUIRY_CANCEL, NULL, 0, false);
        }
    }
    apply_scan();
}

bool bt_wake_learning(void) { return s_learn; }

int bt_wake_learned(bt_wake_seen_t *out, int max)
{
    s_learn_poll_ms = now_ms();
    taskENTER_CRITICAL(&s_seen_lock);
    int n = s_nseen < max ? s_nseen : max;
    memcpy(out, s_seen, n * sizeof(*out));
    taskEXIT_CRITICAL(&s_seen_lock);
    return n;
}

bool bt_wake_host(uint8_t out[6])
{
    if (s_started) {
        memcpy(out, s_host, 6);
    }
    return s_started;
}

void bt_wake_tick(void)
{
    if (s_learn && now_ms() - s_learn_poll_ms > LEARN_IDLE_MS) {
        ESP_LOGI(TAG, "learn mode idle");
        bt_wake_learn(false);
    }
    int64_t at = s_handover_at_ms;
    if (at >= 0 && now_ms() >= at) {
        s_handover_at_ms = -1;
        bt_wake_drop(s_handover_method);
    }
}

void bt_wake_status_json(char *buf, size_t len)
{
    static const char *modes[] = { "reject", "silent", "accept", "probe" };
    static const char *drops[] = { "detach", "reset", "radio", "l2cap" };
    char h[18], p[18];
    fmt_addr(s_bdaddr_read, h);
    fmt_addr(s_pad, p);
    snprintf(buf, len,
             "{\"bd_addr\":\"%s\",\"pad\":\"%s\",\"link_key\":%s,\"mode\":\"%s\","
             "\"scan_policy\":\"%s\",\"radio\":%s,\"scanning\":%s,\"host_up\":%s,"
             "\"holding\":%s,\"held_ms\":%lld,\"hid_ctrl\":%s,\"hid_intr\":%s,\"hid_reports\":%lu,"
             "\"last_hold_ms\":%lld,\"last_disc_reason\":%u,\"handover_s\":%d,"
             "\"handover_method\":\"%s\",\"requests\":%lu,\"pad_requests\":%lu,\"presses\":%lu,"
             "\"ignored\":%lu,\"last_burst_count\":%lu,\"last_burst_span_ms\":%lld,\"cutoff\":%u}",
             h, p, s_have_key ? "true" : "false", modes[s_mode],
             s_policy == BT_SCAN_AUTO ? "auto" : s_policy == BT_SCAN_ON ? "on" : "off",
             s_radio_on ? "true" : "false", s_scanning ? "true" : "false",
             s_host_up ? "true" : "false", s_conn != 0xFFFF ? "true" : "false",
             s_conn != 0xFFFF ? now_ms() - s_conn_at_ms : -1LL, s_ch[0].open ? "true" : "false",
             s_ch[1].open ? "true" : "false", (unsigned long)s_hid_reports, s_last_hold_ms,
             s_last_disc_reason, s_handover_delay_s, drops[s_handover_method],
             (unsigned long)s_requests, (unsigned long)s_pad_requests, (unsigned long)s_presses,
             (unsigned long)s_ignored, (unsigned long)s_last_burst_count, s_last_burst_span_ms,
             s_cutoff);
}
