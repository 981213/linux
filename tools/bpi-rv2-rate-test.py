#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Measure BPI-RV2 physical-port forwarding with AF_PACKET sockets."""

import argparse
import fcntl
import socket
import struct
import threading
import time


ETH_P_ALL = 0x0003
ETH_P_8021Q = 0x8100
ETH_P_TEST = 0x88B6
SOL_PACKET = 263
PACKET_AUXDATA = 8
TP_STATUS_VLAN_VALID = 1 << 4
TP_STATUS_VLAN_TPID_VALID = 1 << 6
SIOCGIFHWADDR = 0x8927
MAGIC = b"BPI-RV2-DPNS-RATE"


def interface_mac(ifname):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        request = struct.pack("256s", ifname.encode())
        result = fcntl.ioctl(sock.fileno(), SIOCGIFHWADDR, request)
    return result[18:24]


def packet_socket(ifname, receive=False):
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                         socket.htons(ETH_P_ALL))
    if receive:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024 * 1024)
        sock.setsockopt(SOL_PACKET, PACKET_AUXDATA, struct.pack("=I", 1))
        sock.settimeout(0.1)
    sock.bind((ifname, 0))
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


def make_frame(src, dst, length, vlan, pcp):
    header = dst + src
    if vlan is not None:
        header += struct.pack("!HH", ETH_P_8021Q, pcp << 13 | vlan)
    header += struct.pack("!H", ETH_P_TEST)
    return (header + MAGIC).ljust(length, b"\0")


def matches(frame, src, dst, vlan, pcp):
    if len(frame) < 14 or frame[:6] != dst or frame[6:12] != src:
        return False
    offset = 12
    ethertype = struct.unpack_from("!H", frame, offset)[0]
    offset += 2
    if vlan is not None:
        if ethertype != ETH_P_8021Q or len(frame) < 18:
            return False
        tci, ethertype = struct.unpack_from("!HH", frame, offset)
        if tci & 0xfff != vlan:
            return False
        offset += 4
    return ethertype == ETH_P_TEST and \
        frame[offset:offset + len(MAGIC)] == MAGIC


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tx", help="host interface connected to ingress port")
    parser.add_argument("rx", help="host interface connected to egress port")
    parser.add_argument("--duration", type=float, default=8.0)
    parser.add_argument("--warmup", type=float, default=1.0)
    parser.add_argument("--length", type=int, default=1400)
    parser.add_argument("--vlan", type=int)
    parser.add_argument("--pcp", type=int, default=0)
    args = parser.parse_args()

    if args.duration <= 2 * args.warmup:
        parser.error("--duration must exceed twice --warmup")
    if not 60 <= args.length <= 1500:
        parser.error("--length must be in the range 60..1500")
    if args.vlan is not None and not 1 <= args.vlan <= 4094:
        parser.error("--vlan must be in the range 1..4094")
    if not 0 <= args.pcp <= 7:
        parser.error("--pcp must be in the range 0..7")
    if args.pcp and args.vlan is None:
        parser.error("--pcp requires --vlan")

    tx_mac = interface_mac(args.tx)
    rx_mac = interface_mac(args.rx)
    frame = make_frame(tx_mac, rx_mac, args.length, args.vlan, args.pcp)
    start = time.monotonic() + 0.25
    measure_start = start + args.warmup
    end = start + args.duration
    measure_end = end - args.warmup
    received = 0
    sent = 0

    with packet_socket(args.tx) as tx, packet_socket(args.rx, True) as rx:
        def receive():
            nonlocal received
            while time.monotonic() < end + 1:
                try:
                    data = receive_frame(rx)
                except socket.timeout:
                    continue
                now = time.monotonic()
                if measure_start <= now < measure_end and \
                        matches(data, tx_mac, rx_mac, args.vlan, args.pcp):
                    received += 1

        thread = threading.Thread(target=receive, daemon=True)
        thread.start()
        while time.monotonic() < start:
            pass
        while time.monotonic() < end:
            tx.send(frame)
            sent += 1
        thread.join()

    interval = measure_end - measure_start
    # Include the preamble, start delimiter, FCS and inter-packet gap so this
    # matches the TMU shaper's configured wire-rate accounting.
    wire_bytes = args.length + 24
    rate = received * wire_bytes * 8 / interval
    print(f"sent={sent} measured_received={received} "
          f"wire_rate={rate / 1_000_000:.3f} Mbit/s")
    return 0 if received else 1


if __name__ == "__main__":
    raise SystemExit(main())
