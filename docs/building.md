# Building and flashing

## Toolchain

ESP-IDF v5.5.6. On this workstation it lives at `~/esp/v5.5.6/esp-idf`, shared with quickvolt. Two
quirks:

- That install is registered for esp32c6 only, so the Xtensa compiler has to be added to PATH by
  hand.
- `PYTHONPATH` has to be unset first.

```
git clone --recursive https://github.com/Nero7991/esp32-WoLviaBT
unset PYTHONPATH
. ~/esp/v5.5.6/esp-idf/export.sh
export PATH="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:$PATH"
idf.py build
```

`components/esp-bringup` and `components/netserial` are submodules. `components/ota` is a copy of
the OTA component from `esp32-s3-dw3110`.

## Optional components

These live in private repos. The build works without them; each is used when present under
`components/`, for example as a symlink:

| Path | Adds |
|---|---|
| `components/esp-power` | CPU frequency scaling and `GET /api/power` |
| `components/esp-power-ledger` | Power-state counters (including the Ethernet power state) in `/api/power` |
| `components/esp-recover` | Power-cycle trigger counted 152 ms after power-on, reset history in the status JSON, HTTP self-check |

Without esp-recover, esp-bringup's own power-cycle counter and a built-in HTTP self-check are used.

## Secrets

`sdkconfig.local` (git-ignored) is added to the sdkconfig defaults when it exists. Its
`CONFIG_OTA_PASSWORD` and an optional `main/wifi_creds_local.h` only seed the settings on a board
that has none. After that, everything is edited on the setup page.

## Over the network (normal path)

```
tools/ota_push.sh <board-ip>     # POST /ota, then waits for GET / to answer
python3 -I components/netserial/host/netserial.py <board-ip> --log run.log   # network console
```

- **Rollback:** a new image must get an IP address within 120 s, or the bootloader rolls back to
  the previous slot. Tested 2026-11-25.
- **Bootloader:** OTA updates the app only. Bootloader settings, such as the log level, take
  effect only after a serial flash.

## Over serial (first flash, or recovery)

The WT32-ETH01 has no USB. Use a USB-serial adapter with 3.3 V logic, or the Raspberry Pi as in
[wiring-rpi-flasher.md](wiring-rpi-flasher.md).

| Adapter | WT32-ETH01 |
|---|---|
| TX | RXD (GPIO3) |
| RX | TXD (GPIO1) |
| GND | GND |
| 5V | 5V (a cheap adapter's 3V3 pin cannot feed the PHY) |
| GND | IO0, only while flashing |

There is no auto-reset circuit:

1. Tie IO0 to GND.
2. Power-cycle the board, or pulse EN to GND.
3. Run `idf.py -p /dev/ttyUSBx flash`.
4. Remove the IO0 jumper and reset.

GPIO0 is also the RMII clock input. The 50 MHz oscillator stays off until the firmware drives
GPIO16 high, which is why strapping IO0 low at boot is safe.
