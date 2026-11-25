# Design: ESP32-S31 as the host's Bluetooth adapter, holding the pad across boot

Status: design only (2026-11-25). Board ordered (ESP32-S31-Function-CoreBoard-1); nothing built.

## Goal

One PS press on a powered-off host (BC-250) both wakes the host and leaves the DualSense connected
to it, with no second press.

## Why the current board cannot do it

Measured in `docs/dualsense-bt-behaviour.md`:

- The pad only ever connects by paging its host after a PS press (`HIDNormallyConnectable` FALSE).
- Unanswered, it pages for about 15 s; the host needs 36 s to reach the network and more for
  Bluetooth.
- Once anything answers its page, every failure (reject, timeout, link loss, clean disconnect,
  refused auth or pairing) powers the pad off without a new page.

So a separate radio (the WT32-ETH01) can detect the press and wake the host, but can never pass the
pad to the host's own adapter. The link has to stay on one radio from the press until the host's
stack takes over.

## Idea

Make that one radio the host's Bluetooth adapter. The ESP32-S31 has a BR/EDR controller and a USB
2.0 OTG controller that runs in device mode. It sits on a host USB port, which stays powered while
the host is off (measured on the bench BC-250), and presents itself as a standard USB Bluetooth
controller (class 0xE0/0x01/0x01), which Linux binds with the generic `btusb` driver.

Firmware between the controller and USB is normally a transparent pipe. While the host is off it
acts as a minimal host itself: it answers the pad, triggers the wake, and holds the connection. When
the host's stack attaches, it hides the controller reset and presents the already-open connection.

```
 DualSense ))) [ S31 BR/EDR controller (blob) ]--VHCI--[ proxy firmware ]--USB BTH--> btusb/BlueZ
                                                              |
                                                              +--> wake (WoL) when the pad pages
```

## Building blocks (status from the feasibility survey, 2026-11-25)

| Piece | Source | Status |
|---|---|---|
| BR/EDR controller | S31 datasheet v0.6; ESP-IDF `components/bt/controller/esp32s31` ("Orca"), blob in `espressif/esp32s31-bt-lib` | present in ESP-IDF v6.1 (preview) and master |
| Controller HCI access | `esp_vhci_host_send_packet` / `esp_vhci_host_register_callback` | same API `main/bt_wake.c` already uses |
| USB Bluetooth class | TinyUSB `src/class/bth/bth_device.c` (`tud_bt_hci_cmd_cb`, `tud_bt_event_send`, `tud_bt_acl_data_send`); `esp_tinyusb` 3.0.1 `TINYUSB_BTH_ENABLED`, lists esp32s31 | exists; no ESP-IDF USB HCI transport, the bridge is ours to write |
| Prior art | `adacsabadev/esp32-ble-sniffer` (S3, LE-only, Windows), TiDAL badge `tusb_bth.c` (S3, LE-only) | TinyUSB BTH + VHCI bridge shown to work; never with BR/EDR, never documented on Linux |
| Wi-Fi + BR/EDR coexistence | ESP-IDF S31 coexistence table | STA + BR/EDR listed stable |
| BR/EDR + USB device at once | - | unknown, first thing to test |

## Firmware states

1. **Bridge** (host stack attached). Forward HCI commands from USB to VHCI and events/ACL back,
   unchanged, except:
   - snoop `HCI_Link_Key_Request_Reply` and `HCI_Link_Key_Notification` for the pad and store the key
     (and the host's chosen settings: page scan parameters, class of device, local name) in NVS;
   - track whether the stack is attached: USB configured plus recent HCI commands, and the
     controller-level host-down signals (USB suspend / bus reset / port power loss).
2. **Standby** (host off, USB powered, no stack). The firmware owns the controller: page scan on the
   adapter's own BD_ADDR with the stored settings. On a Connection Request from the stored pad:
   accept, answer the link key request from NVS, trigger the wake, and hold.
3. **Holding**. Keep the pad's attempt alive at a chosen point and buffer everything the pad sends.
4. **Handover**. When the stack attaches (btusb probes, then BlueZ sends `HCI_Reset` and its init
   sequence), answer the init without touching the live link, then present the connection.

## The hold point

Two candidates, both measured with the WT32-ETH01 (cutoff tests):

| Hold | Pad behaviour | Handover work | Risk |
|---|---|---|---|
| C1: do not answer the pad's L2CAP Connection Request for SDP | waits 50.1 s, then disconnects (0x08) | replay the buffered Connection Request to BlueZ; everything after is native | 50 s must cover wake + boot + bluetoothd; the bench BC-250 reaches ping at 36 s, Bluetooth later. Tight |
| C2: open SDP, do not answer the SDP query | waits indefinitely (172 s observed, light on) | the firmware already answered L2CAP connect/config, so it must replay the pad's SDP channel to BlueZ and translate channel IDs (or close SDP to the pad and let it reopen) | more proxy logic, no time limit |

Plan: implement C1 first (smallest proxy), measure the BC-250's time from magic packet to BlueZ
init on this adapter, then decide whether C2 is needed. Authentication happens after SDP in the
pad's sequence, so in both holds the stored key is not used until after handover; BlueZ supplies it
itself. Standby only needs the key if a later hold point is chosen.

## Hiding the reset from the live link

At attach, btusb and BlueZ send `HCI_Reset`, then reads (version, features, BD_ADDR, buffer sizes)
and writes (event mask, scan enable, class, name, SSP mode, page scan settings). In Handover:

- `HCI_Reset`: do not forward; reply Command Complete success.
- Reads: forward (harmless) or answer from cache.
- Writes that would disturb the link or scan state (scan enable, event mask narrowing): forward only
  if harmless, otherwise acknowledge locally and apply after handover.
- Then inject the pad's connection to BlueZ: `Connection Request` (BlueZ answers Accept, which the
  firmware acknowledges locally with a synthesized `Connection Complete` for the existing handle) or
  a `Connection Complete` directly. Then deliver the buffered ACL from the pad.
- Buffer sizes and `Number_Of_Completed_Packets` must stay consistent with what BlueZ was told.

Open question: how BlueZ reacts to an incoming connection that appears within milliseconds of its
own init. Test against the bench BC-250 with btmon.

## Wake source

- The S31's own Wi-Fi (STA, coexistence listed stable) sending a magic packet that reaches the
  host's wired port. On the bench the BC-250's wired port goes straight to the WT32-ETH01, so either
  route it into the LAN or let the S31 tell the WT32-ETH01 (UDP) to send its raw-frame WoL.
- USB remote wakeup does not wake a host from soft-off (S5), only from suspend.

## One-time setup

The pad must be paired to the S31's BD_ADDR (the S31 is now the host's adapter, so a normal pairing
from BlueZ does it), and the old Realtek adapter removed or disabled so only one adapter answers.

## Test plan

1. Plain adapter: S31 as a transparent USB Bluetooth controller. `btusb` binds, `hciconfig`/
   `bluetoothctl show` sees it, the DualSense pairs and plays through it. Check that BR/EDR and USB
   device mode run together, and that Linux's init gets clean answers.
2. Link key snooping in bridge mode.
3. Standby: host off, page detected, wake sent, pad held at C1.
4. Handover with C1: measure time from press to BlueZ init; confirm the pad ends up connected with
   one press.
5. If C1's 50 s is too short: C2.

## Risks

- The S31 BR/EDR controller blob is new (ESP-IDF support marked preview).
- BlueZ init on a controller whose state it did not set up.
- Linux may suspend or reset the USB device at boot in ways that cut power or force a re-enumeration
  (the firmware must keep the radio up across a USB bus reset).
