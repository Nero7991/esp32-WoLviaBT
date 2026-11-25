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

| E1 | accepted **without** role switch (pad stays central, no Role Change event), SDP and both HID channels open, held 751 ms, then `HCI_Reset` | no re-page in the following 60 s (board kept page scanning with the BC-250's Bluetooth off) |

| V2 | probe mode: on the Connection Request the board resets its controller 5 ms later (half-open link vanishes, no reject sent) and page scans again | one page only, no re-page; light about 22 s (timed) |
| V1 | nothing answers at all (BC-250 Bluetooth off, board not scanning); board starts page scanning 15 s after the press and listens 45 s | no page arrived; light about 15-16 s (timed) |

| R-0D | reject with reason 0x0D (limited resources) | one page, Connection Complete status 0x0D after 79 ms, no re-page; light under 10 s |
| R-0E | reject with reason 0x0E (security reasons) | one page, no re-page; light under 10 s |

| C1 | accept, never answer the pad's L2CAP Connection Request for SDP | no retransmission; pad held the link **50.1 s**, then disconnected (reason 0x08, connection timeout); light on throughout; no re-page |
| C2 | accept, open SDP, never answer the SDP query | no retransmission; pad held the link and kept its light on for **172.6 s and counting**, until the board dropped it with a reset; light off a few seconds later (supervision timeout); no re-page |

| C4 | accept, answer SDP, then **negative** Link Key Request Reply | pad started Secure Simple Pairing 1.6 s later (IO capability NoInputNoOutput, auth requirement 0x04 general bonding, no MITM); board refused (reason 0x18); SSP status 0x05, disconnect reason 0x05; light off quickly; no re-page. Pairing with the BC-250 survived (next press connected normally) |

| L1 | accept, held 14.7 s (6997 HID reports), BC-250 Bluetooth on, then `esp_bt_controller_disable()` (radio off, no LMP detach possible: true link loss) | light off within ~10 s; no page to the BC-250 in 68 s (`bluetoothctl info` polled every 2 s); BlueZ's own connects at +2 s and +20 s: Host is down |

| L2 | accept, held 13.8 s (6655 HID reports), board page scan off, BC-250 Bluetooth on, then `HCI_Disconnect` reason 0x13 (explicit host disconnect) | light off quickly; no page to the BC-250 in 60 s |

So with nothing answering, the pad pages for roughly 15 s after a press and then powers off;
an unanswered or vanished connection attempt (V2) does not extend that. The bench BC-250 needs
36 s from the magic packet to answer ping, so a cold boot cannot catch the pad's paging window
even if the board stays silent.

BlueZ's input plugin also pages the pad by itself after the adapter powers on and periodically
afterwards (journal: `control_connect_cb() ... Host is down` at 12:16:25, 12:16:50, 12:17:50,
12:18:43). None of these ever succeeded. **A DualSense that is not connected is not connectable:
only the pad starts a connection, and only on a PS press. After losing its link (reject, accept
timeout, supervision timeout or reset) it turns off.** A held link therefore cannot be passed to
the host.

## The pad's SDP record and what the HID profile says (2026-11-25)

Dumped from the pad over its connection to the BC-250 with `tools/sdp_dump.py` (full output:
`docs/captures/dualsense-edge-sdp.txt`). DS4 values for comparison are decoded from the raw SDP dump
on psdevwiki (DS4-BT page).

| Attribute | DualSense Edge (measured) | DS4 (psdevwiki) |
|---|---|---|
| 0x0204 HIDVirtualCable | TRUE | FALSE |
| 0x0205 HIDReconnectInitiate | TRUE | TRUE |
| 0x020A HIDRemoteWake | TRUE | TRUE |
| 0x020C HIDSupervisionTimeout | 8000 slots (5 s) | 0x1F40 (5 s) |
| 0x020D HIDNormallyConnectable | **FALSE** | **FALSE** |
| PnP record | VID 0x054C, PID 0x0DF2, version 0x0100 | |

Bluetooth HID Profile 1.1 (adopted spec):
- NormallyConnectable FALSE: the device "may completely shut down its Bluetooth radio when there is
  no active connection"; the host is the side that stays in page scan (5.3.4.14, 5.4.2).
- Host-initiated reconnection "will generally only succeed to devices which expose
  HIDNormallyConnectable TRUE" (section 3).
- ReconnectInitiate: the device "shall not immediately reconnect if the connection is dropped at
  the explicit request of the Bluetooth Host ... unless the device generates new data, for example
  due to user activity" (5.3.4.6). After link loss either side may give up after about 30 s and
  "manual user intervention is acceptable" (5.1.5).
- BlueZ `profiles/input/device.c` maps (ReconnectInitiate TRUE, NormallyConnectable FALSE) to
  device-initiated reconnect, which is why its own connect attempts only ever time out.

So the measurements above are the documented behaviour, not a quirk: only the pad can open a
connection. No report was found of anyone making a DS4 or DualSense reconnect, or move between two
hosts, without a button press. Sony's 2025 multi-host pairing for the DualSense also needs PS plus a
face button to switch.

Vendor reports that touch pairing and power (controllers.fandom wiki, dualsensectl):
- DualSense feature 0x0A writes the host BD_ADDR and link key; 0x09 reads the pad and host
  addresses (DS4: 0x13 / 0x12). They choose which host the pad pages, not when.
- Feature 0x08 "Bluetooth control": State 2 = OFF powers the pad down (`dualsensectl power-off`).
  What State 1 = ON does is not documented. **Tested 2026-11-25 12:33, no effect:** sent to the
  connected pad via hidraw (`tools/ds_bt_control.py`, 47 bytes, CRC32 seed 0x53); the pad stayed
  connected; after `bluetoothctl disconnect` five host connects over 34 s all failed
  (`br-connection-create-socket`, page timeout), same as without it.

Custom firmware is not an option: no DualSense firmware dump or custom firmware has been published;
for the DS4 only partial work exists (fail0verflow could not reach SWD; an unverified serial
bootloader dump guide).

### What the cutoff tests show (2026-11-25)

The pad's firmware has two regimes:
- **Before its page succeeds** it keeps paging for about 15 s (V1).
- **Once a host has answered the page**, any failure ends in power-off with no new page: reject (any
  reason), accept timeout, vanished link, supervision timeout, failed authentication, failed
  pairing. Waiting points can stretch the attempt (C1: 50 s until a connection timeout; C2: the pad
  waits on an unanswered SDP query indefinitely, light on), but they only delay the power-off.

A held pad can therefore serve as a visible "waking" indicator, but nothing found here makes it
page again for the real host. A one-press wake needs the host itself to answer the first page
(e.g. suspend-to-RAM with wake from its own Bluetooth adapter), or the board to remain the pad's
host (relay).

Security note from C4: a device that answers the page with the host's BD_ADDR and has no link key
gets the pad to start Just Works pairing (NoInputNoOutput) with no confirmation on the pad. The
board refuses it; a device that accepted would end up bonded to the pad.

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

## Firmware notes

- After `drop=radio` (`esp_bt_controller_disable`) and `radio=on` (`esp_bt_controller_enable` plus
  the same HCI setup) the controller answered every HCI command and reported the right BD_ADDR, but
  never received the pad's next page. Reboot the board after a radio-off test.

## Captures in this repo

- `docs/captures/dualsense-edge-sdp.txt`: the pad's SDP records (HID and PnP), from
  `tools/sdp_dump.py` run on the BC-250 while the pad was connected.
- `docs/captures/dualsense-edge-reconnect-btmon.txt`: btmon decode of a normal reconnect to BlueZ,
  link key redacted.

## Measurement traps hit

- **A capture leaked the link key once.** btmon prints the Link Key Request Reply key in clear. A
  first decode was shown in a session transcript before being shredded; the published capture
  replaces that line and is checked for the key in every byte format. Re-pairing the pad rotates
  the key.
- **A "successful" host connect that was really the pad paging.** At 12:31:07 `bluetoothctl
  connect` reported success; btmon showed the host's own Create Connection had hit Page Timeout at
  12:31:06.516 and the pad's Connection Request arrived 26 ms later from a PS press. Always check
  in btmon who initiated (Create Connection vs Connect Request).
- **hidraw lookup by HID_UNIQ ran before the pad's hidraw node existed**, so the first two 0x08
  attempts sent nothing. Wait for `/dev/hidrawN` after connect.

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
