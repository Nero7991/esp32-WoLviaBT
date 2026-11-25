# How it works

## Detecting the PS press

A DualSense does not broadcast a button press. A paired pad that is off, when PS is pressed, pages
the Bluetooth address of the host it is paired with, for about 15 s.

The board runs the ESP32's BR/EDR controller over VHCI, with no host stack. At init it sets its own
Bluetooth address to the **host adapter's address**, so the pad's page reaches it. It runs page
scan only: it never appears in a device search.

When a Connection Request arrives from the saved controller address, the board rejects it (reason
0x0F) and counts a press. The pad powers off after a rejection, so a second press is needed once
the host is up.

Page scan runs only while the host is down (scan policy `auto`). While the host is up, the host's
own adapter answers the pad.

Everything tried to avoid the second press is in
[dualsense-bt-behaviour.md](dualsense-bt-behaviour.md): reject reasons, staying silent, accepting
and handing over, link loss and detach, cutting off mid-connection. None makes the pad page again.
The one-press design moves the adapter role onto an ESP32-S31 instead:
[design-s31-adapter.md](design-s31-adapter.md).

## Is the host up?

The board pings the IPv6 all-nodes address `ff02::1` on the Ethernet link every 2 s. A running
Linux host answers from its link-local address with no IP configuration at all; an off host
cannot. Replies from the board's own address are ignored. The host counts as up while a reply
arrived in the last 8 s.

## Waking

The magic packet is a raw Ethernet frame (broadcast, EtherType 0x0842) sent with
`esp_eth_transmit`. It needs only the link: no IP address, DHCP or route, and it cannot leave
through Wi-Fi. On a wake request the board powers Ethernet up if needed, waits for the link, and
sends three packets 300 ms apart.

**Wake tests:**

- **Bench BC-250, 2026-11-25 (r8169, `Wake-on: g`):**
  - each `POST /wol` raised `enp4s0` `rx_packets` by exactly 1, three times out of three;
  - the box was off for 2.5 min and its USB port kept the board powered, with the Ethernet link
    up;
  - it answered ping 36 s after the packet.
- **Basement BC-250, same day:**
  - a PS press with Ethernet powered down sent the packet 12 s later, and the box booted 1 s
    after the packet;
  - `POST /wol` brought the box up in 34 s.

## Settings and recovery

- **Settings:** live in NVS and are managed by
  [esp-bringup](https://github.com/Nero7991/esp-bringup): setup page, hotspot, captive portal,
  Detect dialog.
- **Power cycles, reset history and HTTP self-check:** the optional esp-recover component counts
  the power cycles that bring back setup mode. It also keeps the last 8 reset reasons and runs an
  HTTP self-check that restarts the board if its server stops answering for 3 minutes.

## Hardware

- **WT32-ETH01:**
  - Wireless-Tag WT32-S1 module, an original ESP32. It has Bluetooth Classic, which DualShock 4
    and DualSense use; an ESP32-S3 (BLE only) could not hear them.
  - No native USB: USB is power only.
  - Ethernet: LAN8720, PHY address 1, MDC GPIO23, MDIO GPIO18, 50 MHz oscillator enabled by
    GPIO16 and clocking GPIO0.
- **Host:** BC-250 with its ATX supply, and a DualSense paired with it.

This repo started as `esp32s3-WoLviaBT`, on the assumption the module was an ESP32-S3.

## Future: the board switches the supply (PS_ON)

Not built. The board would run from the ATX 5 V standby rail and switch PS_ON (the green wire) to
ground to turn the 12 V rail on. The BC-250 would be set to power on when 12 V appears.

```
ATX 5VSB (purple) ---------> board (always on)
board GPIO --switch--> ATX PS_ON (green) to GND
ATX 12 V ---------> BC-250 (auto power on)
```

Open points:

- PS_ON must be switched with an open-drain output or a transistor, never driven high from a 3.3 V
  GPIO.
- 5VSB current is limited (typically 2 to 3 A); check nothing else hangs on that rail.
- Cutting 12 V is not a clean shutdown. Turning the supply off when the host has been down for a
  while must tell a shutdown apart from a reboot, the BIOS, or suspend. The current Ethernet host
  check would be the starting point.
- Whether the BC-250's auto power-on works reliably with this supply is untested.
