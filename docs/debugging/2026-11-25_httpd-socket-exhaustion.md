# Board unreachable over HTTP on a new network (2026-11-25)

## Question

On 2026-11-25, why did the WT32-ETH01 stop answering HTTP after it moved from the bench Wi-Fi
to the "MyHome" network (192.168.1.21)? The build was esp32-WoLviaBT d01f142 with esp-bringup,
on ESP-IDF v5.5.6 with default lwIP and httpd settings. Every connection was accepted at TCP level
and then reset, and OTA was impossible. Ping and netserial still worked.

## Answer

The socket table was full. lwIP had 10 sockets in total; httpd used 2 for itself and allowed
7 clients; netserial and the ping session used one more each. A device on MyHome probes every new
host for router APIs (`/JNAP/`, `/HNAP1/`) about 1.7 s after the board gets its address, and
keeps reconnecting every ~3 s. Those connections stayed open and filled the client slots. httpd
never drops idle sessions unless `lru_purge_enable` is set, so every new accept failed with errno
23 (ENFILE). A power cycle cleared it for under 2 seconds.

Fixes:
- `lru_purge_enable = true`
- `CONFIG_LWIP_MAX_SOCKETS=16`
- an HTTP self-check that restarts the board after 3 failed loopback fetches 60 s apart

## Procedure

1. Scanned 192.168.1.x for port 80, then for the board's status JSON. Nothing matched, but .21
   accepted TCP and reset HTTP.
2. Connected netserial (UDP 5514) to .21. It answered, which identified the board and gave its
   log.
3. Read the log: `httpd_accept_conn: error in accept (23)` repeating. Errno 23 is ENFILE: the
   lwIP socket table is full, and httpd's own limit was not what failed.
4. Had the board power-cycled once and read the fresh boot. It got its IP at 5.77 s. `/JNAP/` and
   `/HNAP1/` requests arrived at 7.45 s and the first ENFILE at 7.72 s. So the cause is a client on
   MyHome, not the earlier phone session.
5. Recovery: OTA over Wi-Fi (180 ms RTT) lost the race. A watcher on the BC-250 polled the board
   over the Ethernet cable (IPv6 link-local, ~1 ms) every 0.2 s and pushed the image as soon as
   it answered. Ethernet comes up about 2 s before Wi-Fi, which is ahead of the scanner. It
   worked on the first boot it saw.

## Evidence

```
20:16:00 I (5773) wol: ip 192.168.1.21 gw 192.168.1.1
20:16:00 W (7453) httpd_uri: URI '/JNAP/' not found
20:16:00 W (7483) httpd_uri: URI '/HNAP1/' not found
20:16:01 E (7723) httpd: httpd_accept_conn: error in accept (23)
... every ~3 s after that
BC-250 watcher: 15:21:59 board answered, pushing / OTA OK, rebooting / push rc=0
After the fix, over the Ethernet link with the scanner still active: 200 200 200 200 200 200
```

## Measured and REJECTED (do not retry)

- **"Idle phone captive-portal connections":** that was the first guess, from the same error at
  19:53 during the phone session. Withdrawn: the fresh boot at 20:16 had no phone on the board and
  filled up within 2 s of getting an IP.
- **Cutting the board's USB power from the BC-250:** not possible. The board is on a power-only
  cable, so it never enumerates and its port cannot be identified. The sysfs `disable` attribute
  does not remove VBUS. Per-port power switching on the AMD root hubs is unverified, and it would
  also drop the BC-250's USB Bluetooth/Wi-Fi adapter.

## Measurement traps

- `pkill -f "netserial.py 192.168.97.142"` run inside a `bash -c` whose own command line held
  that string killed the shell itself (exit 144). Run long-lived loggers as background tasks
  instead.
- After the fixed boot, 192.168.1.21 did not answer from the workstation for a few minutes,
  although the board reported that IP and answered over Ethernet. It came back by itself. The cause
  is not determined, and the IP did not change.

## Open

- Which MyHome device sends the JNAP/HNAP probes was not identified.
