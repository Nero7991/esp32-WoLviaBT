# Setup guide

## 1. Wiring

```
BC-250 USB port --5 V--> WT32-ETH01            (power only; the board has no USB data)
BC-250 wired port <--Ethernet cable--> WT32-ETH01
```

- The USB port must stay powered while the box is off. It does on both bench BC-250s.
- The cable goes straight from the board to the box; no switch or IP network is needed.

## 2. Prepare the host

On the BC-250 (or any Linux host with NetworkManager):

```
nmcli con add type ethernet ifname enp4s0 con-name wol-link \
  ipv4.method link-local ipv6.method link-local \
  802-3-ethernet.wake-on-lan magic connection.autoconnect-priority 10
nmcli con mod "Wired connection 1" connection.autoconnect no   # if a DHCP profile exists
nmcli con up wol-link
sudo ethtool enp4s0 | grep Wake-on        # expect "Wake-on: g"
```

The board checks whether the host is up by pinging `ff02::1` on the cable, so the link only needs
an IPv6 link-local address. `ethtool -s ... wol g` alone does not survive a reboot; the profile
does.

Collect two addresses from the host. Both can also be filled in later.

| Setting | Command |
|---|---|
| Host Bluetooth adapter | `bluetoothctl show` (the `Controller` line) |
| Host wired MAC | `ip link show enp4s0` |

Pair the DualSense with the host as usual (hold PS + Create, then `bluetoothctl` or the desktop's
Bluetooth settings).

## 3. First boot: setup mode

A board with any required setting empty starts in setup mode:

1. Join the Wi-Fi hotspot `WoLviaBT-XXXX` with password `wolviabt`. The setup page opens by
   itself; otherwise go to `http://192.168.4.1/setup`.
2. Sign in with the admin password, user `admin`. A brand-new board has none yet: set it on the
   page.
3. Fill in the settings and press **Save and restart**.

| Setting | Notes |
|---|---|
| Wi-Fi network and password | The network the board joins for management and OTA. **Scan** lists nearby networks |
| Device name | Optional |
| Admin password | 8 or more characters. Protects `/setup`, `/ota`, `/wol` and `/bt` |
| Host Bluetooth adapter | From `bluetoothctl show` on the host |
| Controller address | Use **Detect** (below) |
| Controller link key | Optional, only for the accept-mode experiments |
| Host wired MAC | From `ip link` on the host |
| Power settings | See [power.md](power.md); the defaults are fine |

The board also stays on its saved Wi-Fi network in setup mode, so `http://<board-ip>/setup`
works there too. The page is available in normal mode as well.

## 4. Detect the controller

Press **Detect** next to the controller address. The board listens two ways at once:

- **Pairing mode:** hold PS + Create until the light flashes fast. The pad is listed with its name.
  This works without the host adapter address.
- **PS press:** a paired pad calls only its host's adapter address. The board hears it only if the
  host adapter address is already saved, and only if the host's Bluetooth is off (otherwise the
  host answers first). Turn it off with `bluetoothctl power off`, and back on afterwards.

Pick the entry labelled with the controller's name or "Game controller", then save.

Not yet verified: whether pairing mode, left without pairing to anything, keeps the pad's existing
pairing with the host.

## 5. Moving the board to another host

Set the new host adapter address and wired MAC first, save and restart. The board can only hear
the pad once it answers as the new adapter. Then run Detect.

To bring back setup mode on a board you cannot reach:

- **Power cycles:** plug it in for about 2 s and unplug it, three times; then plug it in a fourth
  time and leave it. Each plug-in must be under 10 s. With esp-recover, a plug-in counts 152 ms
  after power-on. On the bench, one plug-in in four was occasionally not recorded; just repeat the
  sequence.
- **Over the network:** `curl -u admin:<pw> -X POST 'http://<board>/api/setup/action?do=bringup'`.
- **Wi-Fi gone:** after 5 failed attempts to join its saved network, the board opens the hotspot by
  itself, next to the retries.

## 6. Everyday use

- **Wake:** press PS on the pad, then press it again once the box is up to connect. Or from any
  computer that can reach the board: `curl -u admin:<pw> -X POST http://<board>/wol`.
- **Check:** `curl http://<board>/` shows `host_up`, `eth_power`, the press count and the
  recovery counters. The full list is in [http-api.md](http-api.md).

Measured on the basement BC-250 with Ethernet powered down:

| Wake | Packet sent | Host booted | Board sees host up |
|---|---|---|---|
| PS press | 12 s after the press | 1 s after the packet | |
| `POST /wol` | | | 34 s after the request |
