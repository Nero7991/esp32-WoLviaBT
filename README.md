# esp32-WoLviaBT

Press the PS button on a DualSense and a powered-off BC-250 gaming box turns on.

A WT32-ETH01 (ESP32 with Ethernet) stays powered from one of the box's USB ports. It listens for
the controller over Bluetooth by taking on the box's own adapter address, and on a press sends a
Wake-on-LAN packet over the Ethernet cable. A second press then connects the pad to the box as
usual.

## Status

- **Working and tested on two BC-250s (2026-11-25):**
  - wake on a PS press, including from Ethernet powered down;
  - wake over HTTP;
  - setup page with controller Detect;
  - OTA updates with rollback;
  - network console.
- **Not possible:** waking and connecting with a single press needs the board to be the box's
  Bluetooth adapter itself; see [docs/design-s31-adapter.md](docs/design-s31-adapter.md) (ESP32-S31,
  planned).

## Documentation

| Document | Contents |
|---|---|
| [Setup guide](docs/setup-guide.md) | Wiring, preparing the host, first-boot setup, Detect, moving the board, recovery |
| [How it works](docs/how-it-works.md) | PS press detection, host check, wake path, hardware, future PS_ON wiring |
| [Power](docs/power.md) | Ethernet power-down, CPU frequency scaling, why there is no light sleep |
| [HTTP API](docs/http-api.md) | Status, setup, wake, Bluetooth and OTA endpoints |
| [Building and flashing](docs/building.md) | Toolchain, OTA, serial flashing, optional components |
| [DualSense behaviour](docs/dualsense-bt-behaviour.md) | Everything measured about the controller's reconnect, with captures |
| [RPi flasher wiring](docs/wiring-rpi-flasher.md) | Flashing over serial from a Raspberry Pi |
| [Debugging records](docs/debugging/) | Root-caused issues, with the dead ends |

## Quick start

1. Build and flash once over serial ([building](docs/building.md)).
2. Power the board from a USB port of the box, and cable its Ethernet to the box's wired port.
3. Join the `WoLviaBT-XXXX` hotspot (password `wolviabt`) and fill in the setup page
   ([setup guide](docs/setup-guide.md)).

## License

MIT. See `LICENSE`.
