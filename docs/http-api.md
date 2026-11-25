# HTTP API

The board serves HTTP on port 80, over both Wi-Fi and the Ethernet cable (IPv6 link-local, e.g.
`http://[fe80::...%25enp4s0]/` from the host).

**Auth:** endpoints marked "admin" use HTTP Basic auth: user `admin`, password = the admin
password from the setup page. A board with no admin password yet accepts requests without auth.

## Status

| Method | Path | Auth | |
|---|---|---|---|
| GET | `/` | none | Status JSON |
| GET | `/api/power` | none | PM locks, light-sleep blockers, time per CPU mode, power counters. Needs the optional esp-power component |

Fields in the status JSON:

| Field | Meaning |
|---|---|
| `version`, `partition` | Firmware version and running OTA slot |
| `ip` | Wi-Fi address |
| `eth_link`, `eth_power` | Ethernet link state and power state (`on`, `off`, `probe`) |
| `wol_target` | Host wired MAC the magic packet is sent to |
| `uptime_s` | Seconds since boot |
| `setup` | Why the board is in setup mode, or empty |
| `recover` | Power-cycle counter and the last 8 reset reasons: 1 power-on, 3 software restart, 4 panic, 9 brownout |
| `bt` | Bluetooth state: `bd_addr`, `pad`, `mode`, `scanning`, `host_up`, `presses` and the counters |

## Setup

These come from esp-bringup.

| Method | Path | Auth | |
|---|---|---|---|
| GET | `/setup` | none | The setup page |
| GET | `/api/setup` | admin | Every setting (secrets are never returned) and the mode |
| POST | `/api/setup` | admin | Body `{"values":{"key":"value",...},"reboot":true}`. All values are validated first; nothing is saved unless all are valid |
| GET | `/api/setup/scan` | admin | Nearby Wi-Fi networks |
| POST | `/api/setup/action?do=reboot` | admin | Restart |
| POST | `/api/setup/action?do=bringup` | admin | Restart into setup mode |
| POST | `/api/setup/detect?key=bt_pad&do=start` | admin | Start Detect |
| POST | `/api/setup/detect?key=bt_pad&do=stop` | admin | Stop Detect |
| GET | `/api/setup/detect?key=bt_pad` | admin | Candidates found so far |

## Wake

| Method | Path | Auth | |
|---|---|---|---|
| POST | `/wol` | admin | Send the magic packet. If Ethernet is powered down, it powers up first and replies before sending |
| POST | `/wol?mac=aa:bb:cc:dd:ee:ff` | admin | Store the target MAC (same as the setup field) |

## Bluetooth (experiments)

`POST /bt` (admin) takes any combination of:

| Parameter | Effect | Stored |
|---|---|---|
| `host=..`, `pad=..` | Addresses. A new host address restarts the board, because its Bluetooth address is fixed at init | yes |
| `mode=reject\|silent\|accept\|probe` | How the board answers the pad's page. `probe` is for measurement only | yes |
| `scan=auto\|on\|off` | Page scan policy | yes |
| `lk=<32 hex>` | Link key for the pad, as in BlueZ's info file. Never echoed | yes |
| `handover=<s>\|off`, `method=detach\|reset\|radio` | Drop a held pad automatically after the host comes up | yes |
| `drop=detach[&reason=hex]\|reset\|radio\|l2cap` | Let go of a held pad now | no |
| `radio=on` | Re-enable the radio after `drop=radio` | no |
| `role=central\|peripheral` | Role requested when accepting | no |
| `reject_reason=<hex>` | Reason used by reject mode, default 0F | no |
| `cutoff=0..6` | Accept-mode measurement stage, see `main/bt_wake.h` | no |

## OTA

| Method | Path | Auth | |
|---|---|---|---|
| POST | `/ota` | admin | Upload a firmware image. A new image must get an IP address within 120 s, or the bootloader rolls back |
