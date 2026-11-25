#!/usr/bin/env python3
"""Send DualSense feature report 0x08 ("Bluetooth control") over Bluetooth via hidraw (root).

Usage: ds_bt_control.py <pad bdaddr> <state>
State 2 = OFF powers the pad down (as `dualsensectl power-off`). State 1 = ON is undocumented.
Layout from dualsensectl: 47 bytes, [0x08, state, 0 ...], CRC32 over (0x53 || bytes 0..42) in the
last 4 bytes, little-endian.
"""
import fcntl
import glob
import struct
import sys
import zlib


def hidiocsfeature(length):
    # _IOC(_IOC_WRITE | _IOC_READ, 'H', 0x06, len)
    return (3 << 30) | (length << 16) | (ord("H") << 8) | 0x06


def find_hidraw(addr):
    """Match HID_UNIQ to the pad's address; BlueZ's uhid path may leave it empty, so fall back
    to the only Bluetooth (bus 0005) Sony (054C) DualSense hidraw device."""
    addr = addr.lower()
    sony = []
    for dev in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        with open(dev + "/device/uevent") as f:
            ev = f.read().lower()
        node = "/dev/" + dev.rsplit("/", 1)[1]
        if f"hid_uniq={addr}" in ev:
            return node
        if "hid_id=0005:0000054c" in ev and "dualsense" in ev:
            sony.append((node, ev))
    if len(sony) == 1:
        print("matched by name (HID_UNIQ:", [l for l in sony[0][1].splitlines() if "uniq" in l], ")")
        return sony[0][0]
    sys.exit(f"no single hidraw device for {addr} ({len(sony)} DualSense candidates)")


def main():
    addr, state = sys.argv[1], int(sys.argv[2], 0)
    buf = bytearray(47)
    buf[0], buf[1] = 0x08, state
    struct.pack_into("<I", buf, 43, zlib.crc32(bytes([0x53]) + bytes(buf[:43])))
    path = find_hidraw(addr)
    with open(path, "r+b", buffering=0) as f:
        n = fcntl.ioctl(f, hidiocsfeature(len(buf)), bytes(buf))
    print(f"{path}: feature 0x08 state {state} sent ({len(buf)} bytes)")


if __name__ == "__main__":
    main()
