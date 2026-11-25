# Woken host never seen as up after an Ethernet power-down (2026-11-25)

## Question

On 2026-11-25, why did the waker never see the host come back after it woke it? The setup was
the WT32-ETH01 waker (esp32-WoLviaBT 031d8ee plus Ethernet power-down, eth_grace 300 s,
eth_poll 30 s) on the basement BC-250 `cachyos-bc250-2`. The PS press at 22:19:29 UTC was
detected, Ethernet came up and the magic packet went out at 22:19:41, and the host booted at
22:19:42. But the board kept `host_up=false`: it powered Ethernet down again after its 3-minute
hold (22:22:29), and every 30 s check after that reported "host still off".

## Answer

The esp_ping session did not survive the Ethernet stop and start. `eth_power()` called
`esp_ping_stop()` before `esp_eth_stop()` and `esp_ping_start()` after `esp_eth_start()`, and after
that the session sent nothing. The fix deletes the session when Ethernet powers down and creates a
new one from the 1 s watch loop once the link is up and the link-local address is valid. Verified:
after a power-down, the next check saw the host 5 s after the link came up.

## Procedure

1. Board status and log: the press, the power-up, the magic packet, then `host_up=false`
   throughout. This rules out the wake path itself.
2. On the host: `uptime -s` gave 17:19:42 local (22:19:42 UTC), one second after the packet. So
   the wake worked.
3. Host NetworkManager journal for `enp4s0`: `wol-link` activated at 22:20:01 and stayed up until
   the board cut the link at 22:22:35. It also re-activated within 0.2 s of carrier on each later
   check. So the host had its link-local address on the cable the whole time.
4. Host ICMPv6 settings, compared with the bench BC-250, where host-up detection works:
   `echo_ignore_multicast/all/anycast` were all 0 on both, and both ran ufw with the same rule
   count. This rules out host-side filtering, at least as a difference between the two hosts.
5. `tcpdump -i enp4s0 icmp6` on the host through one of the board's checks: the board sent DAD
   for its link-local and a router solicitation, and **no echo requests**. This isolates the fault
   to the board's ping session.
6. Fix, then a test without a shutdown: eth_grace set to 0, host `ip link set enp4s0 down`, the
   board powered Ethernet off 9 s later, host link back up, and the next check found the host.

## Evidence

```
board: 22:19:29 presses=1 / 22:19:32 eth=on / 22:19:41 magic packet sent to a8:a1:59:b2:6c:e3
host:  uptime -s 2026-11-25 17:19:42 (EST)
host NM: 17:20:01 enp4s0 Activation: successful (wol-link) ... 17:22:35 carrier-changed (board cut it)
host tcpdump during a check:
  :: > ff02::1:ffbd:1197  neighbor solicitation, who has fe80::6b2:47ff:febd:1197
  fe80::6b2:47ff:febd:1197 > ff02::2  router solicitation          (x2, no echo request)
after the fix:
  22:27:38 eth=off host_up=False / 22:27:39 host link up / 22:28:09 eth=probe / 22:28:14 eth=on host_up=True
```

## Measured and REJECTED (do not retry)

- **"The basement host doesn't wake (WoL off or BIOS)":** wrong. It woke one second after the
  packet. WoL had been off (`Wake-on: d`) only until `wol-link` was added earlier that day.
- **Host ignores multicast echo or a firewall drops it:** the sysctls and the ufw rule count match
  the bench host, which answers. The capture showed the board never sent an echo, so the host was
  never asked.

## Measurement traps

- The bench test of the same feature reported success. But "host up" there was only observed after
  I restarted the board to restore settings, which created a fresh ping session. The stop and
  start path had never been exercised while the host was up.
- Reading the board's `host_up` alone made it look like the host had not woken. Check the host's
  own uptime before suspecting WoL.

## Open

- Why a stopped and restarted esp_ping session sends nothing after an Ethernet stop and start was
  not traced inside ESP-IDF. It may be the socket bound to a netif that went down. Recreating the
  session avoids the question.
