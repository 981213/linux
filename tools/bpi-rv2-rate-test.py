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
ETH_P_TEST = 0x88B6
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
        sock.settimeout(0.1)
    sock.bind((ifname, 0))
    return sock


def make_frame(src, dst, length):
    header = dst + src + struct.pack("!H", ETH_P_TEST)
    return (header + MAGIC).ljust(length, b"\0")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tx", help="host interface connected to ingress port")
    parser.add_argument("rx", help="host interface connected to egress port")
    parser.add_argument("--duration", type=float, default=8.0)
    parser.add_argument("--warmup", type=float, default=1.0)
    parser.add_argument("--length", type=int, default=1400)
    args = parser.parse_args()

    if args.duration <= 2 * args.warmup:
        parser.error("--duration must exceed twice --warmup")
    if not 60 <= args.length <= 1500:
        parser.error("--length must be in the range 60..1500")

    tx_mac = interface_mac(args.tx)
    rx_mac = interface_mac(args.rx)
    frame = make_frame(tx_mac, rx_mac, args.length)
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
                    data = rx.recv(2048)
                except socket.timeout:
                    continue
                now = time.monotonic()
                if (measure_start <= now < measure_end and
                        len(data) >= 14 + len(MAGIC) and
                        data[:6] == rx_mac and data[6:12] == tx_mac and
                        struct.unpack_from("!H", data, 12)[0] == ETH_P_TEST and
                        data[14:14 + len(MAGIC)] == MAGIC):
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
