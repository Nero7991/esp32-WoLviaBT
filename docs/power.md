# Power

## Ethernet power-down while the host is off

The LAN8720 Ethernet chip and its 50 MHz oscillator are the board's largest steady load. They are
needed only to check whether the host is up and to send the magic packet. While the host is off:

1. **Grace period:** Ethernet stays on for `eth_grace` seconds (default 300) after the host was last
   seen, which covers reboots.
2. **Off:** then it powers down. The order is driver stop, PHY power-down over MDIO, oscillator off.
3. **Checks:** every `eth_poll` seconds (default 30) it comes up for a 10 s host check.
4. **Press or wake request:** it comes up at once and stays on for 3 minutes while the host boots.

Settings (on the setup page, group Power):

| Setting | Default |
|---|---|
| Turn Ethernet off while the host is off (`eth_save`) | on |
| Host check interval while off (`eth_poll`) | 30 s |
| Keep Ethernet on after the host goes off (`eth_grace`) | 300 s |

**Measured:**

| | Result |
|---|---|
| Ethernet on-time while the host was off, 30 s settings | about 25 % |
| Press to magic packet, Ethernet off | about 3.3 s |
| Next check after the host came back | saw it within 5 s |

**Trade-off:** if the host is started another way, the board can believe it is off for up to
`eth_poll` seconds. A press in that window is answered by the board and the pad gives up. The press
also brings Ethernet up, so the next press connects normally.

## CPU frequency and light sleep

Light sleep is not possible on this board:

| Lock | Held | Why |
|---|---|---|
| `emac_esp32` (APB_FREQ_MAX) | 100 % | Taken at Ethernet driver install, released only at uninstall, not by `esp_eth_stop()` |
| `btLS` (NO_LIGHT_SLEEP) | about 99 % | The BR/EDR controller has no 32 kHz crystal for its sleep clock |
| `wifi` (APB_FREQ_MAX) | about 30 % | Station with modem sleep, busy with traffic |

With the optional `esp-power` component the CPU scales between 80 and 160 MHz instead of a fixed
160. It spent about 93 % of the time at 80 MHz. It never reaches 40 MHz, because of the Ethernet
lock. Releasing that lock would mean reinstalling the Ethernet driver on every cycle.

`GET /api/power` (with esp-power) lists every PM lock, which ones block light sleep, the held
percentages and the time per CPU mode. Read the profiled percentages: an instantaneous sample
over Wi-Fi is biased, because the request itself wakes the radio and the CPU.
