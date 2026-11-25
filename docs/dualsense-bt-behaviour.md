# DualSense behaviour when its host is off, and how the board detects the PS button

Living record of what has been measured. Append new results with a date; do not rewrite old ones.

## Setup (2026-11-25)

- Controller: DualSense Edge, BD_ADDR `AC:36:1B:AD:7F:4B`, Class of Device `0x002508`
  (peripheral, gamepad). Paired with the bench BC-250 (`cachyos-bc250`), whose USB Bluetooth adapter
  is `00:E0:4C:23:99:87` (Realtek, manufacturer 93). BlueZ stores a link key for the pad (`Type=4`).
- Board: WT32-ETH01 (ESP32-D0WD rev 1.1), ESP-IDF v5.5.6, BR/EDR controller only, driven over VHCI by
  `main/bt_wake.c` (no host stack). BD_ADDR overridden with `esp_iface_mac_addr_set(addr, ESP_MAC_BT)`
  before controller init; `HCI_Read_BD_ADDR` confirms `00:E0:4C:23:99:87`.
- Page scan only (`Write_Scan_Enable 0x02`, never inquiry scan), interlaced, interval 0x0400 (640 ms),
  window 0x0024 (22.5 ms).
- To stop the real host answering during tests, its adapter was powered off with
  `bluetoothctl power off`; the BC-250 itself stayed up.
- All HCI events logged with ESP timestamps over netserial (`logs/bt-YYYYMMDD.log`, not committed).

## What the pad sends

It does not broadcast or advertise. A press with the pad off makes it **page the BD_ADDR of the host it
was last paired with**. On the board this arrives as one HCI Connection Request:

```
16:55:19.951 bt_wake: CONN_REQ t=91871 ms from AC:36:1B:AD:7F:4B cod=0x002508 link=1 PAD burst#1 +0 ms
16:55:19.951 bt_wake: PRESS #1
16:55:19.951 wol: magic packet sent to a8:a1:59:b2:0b:0a
```

Filtering on the pad's BD_ADDR is enough; no other device has paged the board so far.

## Measured: how long the pad stays on after one press

| Test | Board's answer to the Connection Request | Requests from the pad | Attempt ended | Light on (by eye) |
|---|---|---|---|---|
| 1, reject | `HCI_Reject_Connection_Request`, reason 0x0F, 2 ms after the request | 1 | Connection Complete status 0x0F after 72 ms | under 10 s |
| 2, silent | no answer; `Connection_Accept_Timeout` = 0xB540 (29 s, the spec maximum) | 1 | Connection Complete status 0x08 after **29.1 s** (t=145909 -> 175054 ms) | 10 to 30 s |

In both cases the pad **did not page again** after the attempt ended. One press = one page = one
Connection Request. The pad stays awake while its connection attempt is pending, so the board can
hold it for at most 29 s by not answering, and then it turns itself off.

The bench BC-250 needs 36 s from the magic packet to answering ping (WoL test, README), and more until
`bluetoothd` is up, so even the 29 s hold is not enough for the pad to connect to the real adapter by
itself. A second press is needed today.

## Measured: accepting the connection as the host (2026-11-25)

The board can complete the connection itself using the link key BlueZ stored when the pad was
paired with the BC-250 (`/var/lib/bluetooth/<adapter>/<pad>/info`, `[LinkKey] Key=`, used byte for
byte in `HCI_Link_Key_Request_Reply`). Reference sequence first captured with `btmon` on the
BC-250 during a normal reconnect, then reproduced by the board:

| Step (pad initiates everything) | BlueZ (btmon) | Board |
|---|---|---|
| Connection Request, host accepts with role switch to central | yes | yes |
| Pad sets link supervision timeout 0x1F40 (5 s) | - | +13 ms |
| **Pad opens SDP (PSM 0x0001) before authenticating** | yes | +25 ms |
| One ServiceSearchAttributeRequest: UUID L2CAP (0x0100), attributes 0x0000-0xFFFF, max 2048 | BlueZ returns 1693 bytes in 3 PDUs | board returns an empty list; the pad accepts it |
| Pad closes SDP, then Link Key Request | yes | +68 ms |
| Encryption on (E0, key size 16) | yes | +115 ms |
| HID control PSM 0x11 open | yes | +124 ms |
| HID interrupt PSM 0x13 open | yes | +137 ms |
| Input reports `A1 01 ...` (simple mode) start | yes | +141 ms |

First attempt without an SDP responder: the board refused PSM 1 and the pad disconnected after
105 ms (reason 0x13). With the empty SDP answer the pad stayed connected with a steady light; held
for 20 s (11347 reports) and 14 s (7581 reports) in two runs.

## Measured: handing a held pad to the real host does not work

| Test | What the board did | Result |
|---|---|---|
| 4 | held 20 s, BC-250 Bluetooth powered on, board scan off, then `HCI_Reset` (link vanishes, no LMP detach) | pad light off within a few seconds; no reconnect to the BC-250 in 46 s |
| 5 | held 14 s, BC-250 Bluetooth on, then `HCI_Reset` and at once `bluetoothctl connect` on the BC-250 | `Failed to connect: br-connection-create-socket`; bluetoothd: `connect to AC:36:1B:AD:7F:4B: Host is down (112)` at 12:18:49.29, i.e. page timeout |

BlueZ's input plugin also pages the pad by itself after the adapter powers on and periodically
afterwards (journal: `control_connect_cb() ... Host is down` at 12:16:25, 12:16:50, 12:17:50,
12:18:43). None of these ever succeeded. **A DualSense that is not connected is not connectable:
only the pad starts a connection, and only on a PS press. After losing its link (reject, accept
timeout, supervision timeout or reset) it turns off.** A held link therefore cannot be passed to
the host.

## Host-up detection (decides when the board must stop answering for the host's address)

Two devices with the same BD_ADDR must not page-scan at once, so the board scans only while the host
is down. Method: ping the IPv6 all-nodes address `ff02::1` on the Ethernet link from the board's
link-local address every 2 s; host up = any reply (not from the board itself) in the last 8 s.
This needs no IPv4, DHCP or other configuration on the host.

```
BC-250 normal:                               host_up True
BC-250 sysctl net.ipv6.icmp.echo_ignore_multicast=1:   host_up False (within 12 s)
restored:                                    host_up True
```

## Measurement traps hit

- **IPv4 gateway ping as the host check was wrong.** The bench BC-250's `enp4s0` address
  (192.168.60.1) and its dnsmasq were started by hand in another session and did not survive the
  reboot after the WoL test. With no DHCP the board had no IPv4 on Ethernet, the host looked down,
  and the board page-scanned on the same BD_ADDR as the running BC-250. Replaced by the `ff02::1`
  check above.
- **NetworkManager removes the IPv6 link-local address while it retries DHCP.** `enp4s0` is under
  the automatic "Wired connection 1" profile. Each DHCP attempt times out after 45 s, the device goes
  `failed -> disconnected`, and avahi logs the link-local address being withdrawn. During that window
  the host looks down. It made test 2's press count as "ignored: 46 s after host went down" (the
  post-shutdown filter), so no magic packet was sent in test 2. Fix on the host: give `enp4s0` a
  profile that cannot fail (link-local only), see open items.
- The 11:55:19 local NetworkManager event and the 16:55:19 UTC press line up only because the
  BC-250 logs local time and netserial logs UTC; they are the same second, which looked like a
  causal link. It was not established as one.

## Open, not yet answered

- Whether a clean `HCI_Disconnect` with another reason (0x13, 0x14, 0x15) behaves differently from
  link loss. Expected to also turn the pad off; not tried.
- Whether the board can drive the light bar / player LEDs / rumble on a held pad (output report
  0x31 with CRC32), e.g. to signal "host is up, press PS again".
- Behaviour of a DualShock 4 or another pad.
- Wi-Fi / Bluetooth coexistence on the one radio: no missed press so far (2 of 2 detected), but
  only 2 presses have been tried.
