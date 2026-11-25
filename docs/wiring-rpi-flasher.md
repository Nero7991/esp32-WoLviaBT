# Wiring: RPi 4 (cu-rpi4) as the WT32-ETH01 flasher

Six female-to-female jumpers. Match WT32-ETH01 pins by their silkscreen labels, not by position.

```
        Raspberry Pi 4 GPIO header                         WT32-ETH01
   (pin 1 is at the end away from the USB ports)

           3V3  (1) (2)  5V   ----------------------------  5V
         GPIO2  (3) (4)  5V
         GPIO3  (5) (6)  GND  ----------------------------  GND
         GPIO4  (7) (8)  GPIO14 TXD  ---------------------  RXD  (GPIO3)
           GND  (9) (10) GPIO15 RXD  ---------------------  TXD  (GPIO1)
        GPIO17 (11) (12) GPIO18
        GPIO27 (13) (14) GND
        GPIO22 (15) (16) GPIO23  -------------------------  IO0
           3V3 (17) (18) GPIO24  -------------------------  EN
        GPIO10 (19) (20) GND
           ...  ...  ...

                                                            3V3  (leave unconnected)
```

| Pi pin | Pi signal | WT32-ETH01 | Purpose |
|---|---|---|---|
| 2 | 5V | 5V | Power |
| 6 | GND | GND | Common ground |
| 8 | GPIO14 (TXD) | RXD | Pi transmits, board receives |
| 10 | GPIO15 (RXD) | TXD | Board transmits, Pi receives |
| 16 | GPIO23 | IO0 | Held low at reset to enter download mode |
| 18 | GPIO24 | EN | Pulsed low to reset the board |

Notes:

- TX goes to RX and RX goes to TX. A swapped pair is the most common reason flashing fails to connect.
- Do not connect the board's 3V3 pin. Power it from 5V only, and never from two sources at once.
- Both sides use 3.3 V logic, so no level shifter is needed.
- The Ethernet cable can stay plugged in while flashing.

Pi side, already configured on 2026-11-25: `/dev/serial0 -> ttyAMA0`, serial console removed,
esptool in `~/esptool-venv`. Details in `~/GitHub/DevOps/docs/notes/bench-instruments.md` on the
workstation.
