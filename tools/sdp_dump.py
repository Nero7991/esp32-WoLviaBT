#!/usr/bin/env python3
"""Dump a Bluetooth Classic device's SDP records over a raw L2CAP socket (Linux, BlueZ).

Usage: sdp_dump.py <bdaddr> [uuid16, default 0x0100 = L2CAP, i.e. every record]
The device must be connectable or already connected (a DualSense only while it is on and
connected to this host). Prints every attribute, and names the HID ones (0x0200-0x020F).
"""
import socket
import struct
import sys

PNP_ATTRS = {
    0x0200: "SpecificationID", 0x0201: "VendorID", 0x0202: "ProductID", 0x0203: "Version",
    0x0204: "PrimaryRecord", 0x0205: "VendorIDSource",
}
UNIVERSAL = {
    0x0000: "ServiceRecordHandle", 0x0001: "ServiceClassIDList", 0x0004: "ProtocolDescriptorList",
    0x0006: "LanguageBaseAttributeIDList", 0x0009: "BluetoothProfileDescriptorList",
    0x000D: "AdditionalProtocolDescriptorLists", 0x0100: "ServiceName",
    0x0101: "ServiceDescription", 0x0102: "ProviderName",
}
HID_ATTRS = {
    0x0200: "HIDDeviceReleaseNumber", 0x0201: "HIDParserVersion", 0x0202: "HIDDeviceSubclass",
    0x0203: "HIDCountryCode", 0x0204: "HIDVirtualCable", 0x0205: "HIDReconnectInitiate",
    0x0206: "HIDDescriptorList", 0x0207: "HIDLANGIDBaseList", 0x0208: "HIDSDPDisable",
    0x0209: "HIDBatteryPower", 0x020A: "HIDRemoteWake", 0x020B: "HIDProfileVersion",
    0x020C: "HIDSupervisionTimeout", 0x020D: "HIDNormallyConnectable", 0x020E: "HIDBootDevice",
    0x020F: "HIDSSRHostMaxLatency", 0x0210: "HIDSSRHostMinTimeout",
}


def parse(buf, i):
    """Parse one SDP data element at buf[i]; return (value, next_index)."""
    d = buf[i]
    typ, sz = d >> 3, d & 7
    i += 1
    if typ == 0:
        return None, i
    if sz < 5:
        n = [1, 2, 4, 8, 16][sz]
    else:
        lb = [1, 2, 4][sz - 5]
        n = int.from_bytes(buf[i:i + lb], "big")
        i += lb
    raw = buf[i:i + n]
    i += n
    if typ in (1, 2):  # unsigned / signed int
        return int.from_bytes(raw, "big", signed=(typ == 2)), i
    if typ == 3:
        return "uuid:" + raw.hex(), i
    if typ == 4:
        return raw.decode("latin-1"), i
    if typ == 5:
        return bool(raw[0]), i
    if typ in (6, 7):
        out, j = [], 0
        while j < len(raw):
            v, j = parse(raw, j)
            out.append(v)
        return out, i
    return raw.hex(), i


def main():
    addr = sys.argv[1]
    uuid = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x0100
    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP)
    s.settimeout(10)
    s.connect((addr, 1))
    pattern = bytes([0x35, 3, 0x19]) + struct.pack(">H", uuid)
    attrs = bytes([0x35, 5, 0x0A, 0x00, 0x00, 0xFF, 0xFF])
    cont, data, tid = b"\x00", b"", 1
    while True:
        params = pattern + struct.pack(">H", 0xFFFF) + attrs + cont
        s.send(struct.pack(">BHH", 0x06, tid, len(params)) + params)
        rsp = s.recv(4096)
        if rsp[0] != 0x07:
            sys.exit(f"SDP error response: {rsp.hex()}")
        n = struct.unpack(">H", rsp[5:7])[0]
        data += rsp[7:7 + n]
        cont = rsp[7 + n:]
        tid += 1
        if cont[0] == 0:
            break
    records, _ = parse(data, 0)
    for rec in records:
        attrs = dict(zip(rec[0::2], rec[1::2]))
        classes = attrs.get(0x0001, [])
        table = PNP_ATTRS if "uuid:1200" in classes else HID_ATTRS
        print("--- record", " ".join(classes))
        for aid, val in attrs.items():
            name = UNIVERSAL.get(aid) or table.get(aid, "")
            if isinstance(val, int) and aid >= 0x0200 and table is PNP_ATTRS:
                val = f"0x{val:04X}"
            if aid == 0x0206:
                val = "<HID report descriptor, %d bytes>" % len(val[0][1])
            print(f"  0x{aid:04X} {name:24s} {val}")


if __name__ == "__main__":
    main()
