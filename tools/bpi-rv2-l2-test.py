#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise BPI-RV2 physical-port L2 forwarding with AF_PACKET sockets."""

import argparse
import fcntl
import socket
import struct
import threading
import time

ETH_P_ALL = 0x0003
ETH_P_8021Q = 0x8100
ETH_P_TEST = 0x88B5
SOL_PACKET = 263
PACKET_AUXDATA = 8
TP_STATUS_VLAN_VALID = 1 << 4
TP_STATUS_VLAN_TPID_VALID = 1 << 6
SIOCGIFHWADDR = 0x8927
MAGIC = b"BPI-RV2-DPNS-L2"


def interface_mac(ifname):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        request = struct.pack("256s", ifname.encode())
        result = fcntl.ioctl(sock.fileno(), SIOCGIFHWADDR, request)
    return result[18:24]


def packet_socket(ifname):
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                         socket.htons(ETH_P_ALL))
    sock.setsockopt(SOL_PACKET, PACKET_AUXDATA, struct.pack("=I", 1))
    sock.bind((ifname, 0))
    sock.settimeout(0.1)
    return sock


def receive_frame(sock):
    frame, ancdata, _flags, _addr = sock.recvmsg(2048, 256)
    for level, kind, data in ancdata:
        if level != SOL_PACKET or kind != PACKET_AUXDATA or len(data) < 20:
            continue
        status, _length, _snaplen, _mac, _net, tci, tpid = \
            struct.unpack_from("=IIIHHHH", data)
        if not status & TP_STATUS_VLAN_VALID:
            continue
        if len(frame) >= 14 and struct.unpack_from("!H", frame, 12)[0] != \
                ETH_P_8021Q:
            if not status & TP_STATUS_VLAN_TPID_VALID:
                tpid = ETH_P_8021Q
            frame = frame[:12] + struct.pack("!HH", tpid, tci) + frame[12:]
        break
    return frame


def make_frame(src, dst, direction, sequence, vlan):
    header = dst + src
    if vlan is not None:
        header += struct.pack("!HH", ETH_P_8021Q, vlan)
    header += struct.pack("!H", ETH_P_TEST)
    payload = MAGIC + struct.pack("!BI", direction, sequence)
    return (header + payload).ljust(60, b"\0")


def matches(frame, src, dst, direction, vlan):
    if len(frame) < 14 or frame[:6] != dst or frame[6:12] != src:
        return False
    offset = 12
    ethertype = struct.unpack_from("!H", frame, offset)[0]
    offset += 2
    if vlan is not None:
        if ethertype != ETH_P_8021Q or len(frame) < 18:
            return False
        tci, ethertype = struct.unpack_from("!HH", frame, offset)
        if tci & 0xFFF != vlan:
            return False
        offset += 4
    if ethertype != ETH_P_TEST:
        return False
    return frame[offset:offset + len(MAGIC) + 1] == MAGIC + bytes([direction])


def transfer(tx, rx, src, dst, direction, count, vlan, timeout):
    received = 0
    stop = threading.Event()

    def receive():
        nonlocal received
        deadline = time.monotonic() + timeout
        while received < count and time.monotonic() < deadline:
            try:
                frame = receive_frame(rx)
            except socket.timeout:
                continue
            if matches(frame, src, dst, direction, vlan):
                received += 1
        stop.set()

    thread = threading.Thread(target=receive, daemon=True)
    thread.start()
    for sequence in range(count):
        tx.send(make_frame(src, dst, direction, sequence, vlan))
        if sequence % 32 == 31:
            time.sleep(0.001)
    stop.wait(timeout)
    thread.join()
    return received


def main():
    global ETH_P_TEST

    parser = argparse.ArgumentParser()
    parser.add_argument("left", help="host interface connected to board eth0")
    parser.add_argument("right", help="host interface connected to board eth5")
    parser.add_argument("--count", type=int, default=256)
    parser.add_argument("--vlan", type=int)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--ethertype", type=lambda value: int(value, 0),
                        default=ETH_P_TEST)
    parser.add_argument("--skip-warmup", action="store_true",
                        help="do not prime switchdev FDB learning")
    args = parser.parse_args()

    if not 0x0600 <= args.ethertype <= 0xFFFF:
        parser.error("--ethertype must be in the range 0x0600..0xffff")
    ETH_P_TEST = args.ethertype

    if args.vlan is not None and not 1 <= args.vlan <= 4094:
        parser.error("--vlan must be in the range 1..4094")

    left_mac = interface_mac(args.left)
    right_mac = interface_mac(args.right)
    with packet_socket(args.left) as left, packet_socket(args.right) as right:
        # Exercise both unknown-destination paths before the measured runs so
        # the Linux bridge can publish both source addresses to switchdev.
        if not args.skip_warmup:
            transfer(left, right, left_mac, right_mac, 0, 4, args.vlan,
                     args.timeout)
            transfer(right, left, right_mac, left_mac, 1, 4, args.vlan,
                     args.timeout)
            time.sleep(0.1)
        forward = transfer(left, right, left_mac, right_mac, 0, args.count,
                           args.vlan, args.timeout)
        reverse = transfer(right, left, right_mac, left_mac, 1, args.count,
                           args.vlan, args.timeout)

    print(f"{args.left} -> {args.right}: {forward}/{args.count}")
    print(f"{args.right} -> {args.left}: {reverse}/{args.count}")
    return 0 if forward == args.count and reverse == args.count else 1


if __name__ == "__main__":
    raise SystemExit(main())
