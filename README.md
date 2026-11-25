# esp32-WoLviaBT

Firmware for a WT32-ETH01 (ESP32 with Ethernet) that turns on a BC-250 gaming box when the PS button on a
PlayStation controller is pressed.

The BC-250 is an AMD mining card repurposed as a Steam gaming box. It cannot wake itself from a
controller when it is powered off, because its Bluetooth and USB are unpowered or asleep. This
board stays powered all the time, listens for the controller over Bluetooth, and wakes the box.

Status: firmware runs on the board with OTA updates, rollback and a network console (netserial).
OTA and rollback are tested over Wi-Fi. Ethernet link and the Wake-on-LAN packet are not yet
tested on hardware. Bluetooth detection is not written.

## What the board does

1. It is always powered from 5 V (a USB port or the ATX standby rail).
2. It listens for a PS button press on a paired controller.
3. On a press it powers the BC-250 on. How it does that depends on the wiring scenario below.

## Scenario 1: ATX supply always on, wake by Wake-on-LAN

This is the first scenario to build.

- The ATX supply is forced on permanently: the green wire (PS_ON) is jumpered to ground, so the
  BC-250 always has 12 V.
- Because the BC-250 always has power, its USB ports are powered. The board takes its 5 V from one
  of them.
- The board's Ethernet port is on the same LAN as the BC-250's wired port.
- On a PS button press the board sends a Wake-on-LAN magic packet to the BC-250's wired MAC
  address.

```
ATX supply (PS_ON jumpered to GND) --12 V--> BC-250
BC-250 USB port --5 V--> ESP32 Ethernet board
ESP32 Ethernet board --Ethernet--> LAN <--Ethernet-- BC-250

PS button --Bluetooth--> ESP32 --magic packet--> BC-250 wakes
```

Requirements on the BC-250 side:

- The onboard Ethernet port must be cabled. At the time of writing the box runs on a USB Wi-Fi
  adapter and the wired port is unplugged.
- Wake-on-LAN must be enabled on the wired interface and persist across boots
  (`ethtool -s enp4s0 wol g`). The interface reports support for magic-packet wake, but it is
  currently disabled.
- The firmware and BIOS must honor the magic packet in the power state the box is left in. This
  is untested on the BC-250.
- The USB port feeding the board must stay powered in that state. Also untested.

## Scenario 2: ATX supply switched by the board

This is the more complicated scenario and may be tested later.

- The board runs from the ATX supply's 5 V standby rail (5VSB, the purple wire), which is live
  whenever the supply has mains power.
- The board controls the supply: it pulls the green wire (PS_ON) to ground to turn the 12 V rail
  on, and releases it to turn the rail off.
- The BC-250 is set to power on automatically when 12 V appears, so turning the supply on boots
  the box.
- The board also needs a way to tell whether the BC-250 is alive. The original plan was a USB
  data connection (D+ and D-), but this chip has no native USB (see Hardware). Candidates: sense
  the 5 V of a BC-250 USB port on a GPIO, watch the Ethernet link, or ping the box.

Behavior:

1. Idle: the supply is off, only 5VSB is live, the board is listening for the PS button.
2. PS button pressed: the board pulls PS_ON low, 12 V comes up, the BC-250 boots.
3. Running: the board keeps checking that the BC-250 is alive.
4. Shutdown, either of:
   - The board sees the BC-250 has been down for 1 minute and releases PS_ON, removing 12 V.
   - Software on the BC-250 tells the board to cut power in a given number of minutes.

```
ATX 5VSB (purple) ---------> ESP32 Ethernet board (always on)
ESP32 GPIO --switch--> ATX PS_ON (green) to GND
ATX 12 V ---------> BC-250 (auto power on)
BC-250 USB 5 V ---divider---> ESP32 GPIO (alive monitoring, one candidate)
BC-250 <--Ethernet--> ESP32 (shutdown requests, ping)
```

## Building and flashing

Toolchain: ESP-IDF v5.5.6 at `~/esp/v5.5.6/esp-idf` (shared with quickvolt). That install is
registered for esp32c6 only, so `export.sh` does not put the Xtensa compiler on PATH; add it by
hand. The global `PYTHONPATH` on this machine also has to be unset first.

```
unset PYTHONPATH
. ~/esp/v5.5.6/esp-idf/export.sh
export PATH="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:$PATH"
idf.py menuconfig        # "WoL via BT": target MAC, optional send-on-boot test
idf.py build
idf.py -p /dev/ttyUSBx flash monitor
```

### Over the network (normal path)

```
tools/ota_push.sh <board-ip>     # uploads build/esp32-wol-via-bt.bin to POST /ota, then checks GET /
python3 -I components/netserial/host/netserial.py <board-ip> --log run.log   # network console
curl http://<board-ip>/          # version, running slot, ip, uptime
```

- `POST /ota` uses HTTP Basic auth, user `admin`. The password is in `sdkconfig.local`
  (git-ignored, `CONFIG_OTA_PASSWORD`); without it the endpoint is not registered.
- A new image must get an IP address within 120 s or the bootloader rolls back to the previous
  slot. Tested 2026-11-25: a deliberately network-less image rolled back after about 2 min.
- Wi-Fi station is compiled in only when the git-ignored `main/wifi_creds_local.h` exists (it maps
  the quickvolt bench credentials, see the netserial example). Ethernet is always on.
- `components/ota` is a copy of the OTA component from `esp32-s3-dw3110` (branch `anchor-survey`,
  last component change `cc42d07`). `components/netserial` is a git submodule; clone with
  `--recursive` or run `git submodule update --init`.

### Over serial (first flash, or recovery)

The WT32-ETH01 has no USB. Flash it through an external USB-serial adapter set to 3.3 V logic, or through the Raspberry
Pi `cu-rpi4` as in `docs/wiring-rpi-flasher.md`:

| Adapter | WT32-ETH01 |
|---|---|
| TX | RXD (GPIO3) |
| RX | TXD (GPIO1) |
| GND | GND |
| 5V | 5V (the 3V3 pin of a cheap adapter cannot feed the PHY) |
| GND | IO0, only while flashing |

There is no auto-reset circuit. Tie IO0 to GND, power-cycle or pulse EN to GND, flash, then
remove the IO0 jumper and reset. GPIO0 is also the RMII clock input; the 50 MHz oscillator stays
off until firmware drives GPIO16 high, which is why strapping IO0 low at boot is safe.

Ethernet: LAN8720, PHY address 1, MDC GPIO23, MDIO GPIO18, oscillator enable GPIO16, clock in
on GPIO0.

## Open questions

These need answers before or during implementation. None has been verified.

### How the PS button press is detected

A PlayStation controller does not broadcast a button press. When the PS button is pressed on a
controller that is off, it tries to reconnect to the host it was last paired with. Two ways to use
that:

- The board presents itself with the Bluetooth address of the BC-250's own adapter, so the
  controller's reconnect attempt reaches the board. The board treats the attempt as the wake
  signal and does not complete the connection, leaving the controller free to connect to the
  BC-250 once it is up. This depends on timing and on the controller retrying long enough.
- The controller is paired with the board itself. Detection is simple, but the controller is then
  paired to the wrong host for gaming.

The first approach is the intended one. Whether a controller keeps retrying for as long as the
BC-250 takes to boot has not been measured.

### Scenario 2 details

- PS_ON must be switched with an open-drain output or a transistor, never driven high from a
  3.3 V GPIO.
- 5VSB current is limited (typically 2 to 3 A on a modern supply); the board's draw is small, but
  confirm nothing else hangs on that rail.
- Cutting 12 V removes power without a clean shutdown. The 1 minute "down" rule has to
  distinguish a box that has shut down from one that is rebooting, sitting in the BIOS, or
  suspended.
- Which alive check to use is not decided. Sensing USB 5 V is simple but only shows that the
  rail is up, not that the system is running; a ping shows the system is up but fails during
  boot and in the BIOS.
- The BC-250 would ask for a delayed power cut with a network request to the board. The protocol
  is not decided.
- Whether the BC-250's auto power on setting works reliably with this supply is untested.

## Hardware

- **WT32-ETH01**: Wireless-Tag board with a WT32-S1 module, which is an original ESP32 (not an
  ESP32-S3), and a LAN8720 Ethernet PHY.
  - It has Bluetooth Classic (BR/EDR), which DualShock 4 and DualSense controllers use. An
    ESP32-S3 has BLE only and could not hear these controllers.
  - It has no native USB, so it cannot appear as a USB device to the BC-250. USB is used for
    power only.
- BC-250 board with its ATX power supply.
- A PlayStation controller, paired with the BC-250.
- Scenario 2 only: a transistor or open-drain driver for PS_ON, and a resistor divider if USB 5 V
  is sensed on a GPIO.

This repo was first created as `esp32s3-WoLviaBT` on the assumption that the board was an
ESP32-S3; it was renamed once the module was read as WT32-S1.
