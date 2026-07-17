#!/usr/bin/env python3
"""Power-cycle a NOR-boot BPI-RV2 and boot a FIT image over TFTP."""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import serial


AUTOBOOT_MARKERS = (
    b"hit any key to stop autoboot",
    b"press any key to stop autoboot",
    b"press any key",
)
SHELL_RE = re.compile(
    rb"(?:^|[\r\n])[^#$\r\n]{0,200}[#$][ \t]*(?=\r|\n|$|\x1b)"
)
RC_RE = re.compile(rb"__BPI_RV2_COMMAND_RC__=(\d+)")
BOOT_FAILURES = (
    b"bad data hash",
    b"can't get kernel image",
    b"wrong image format for bootm",
)
WRONG_DTB_MARKER = b"machine model: bananapi bpi-rv2 (booting from nand)"


def check_boot_output(data: bytes) -> None:
    if WRONG_DTB_MARKER in data.lower():
        raise RuntimeError("kernel booted with the NAND DTB; expected the NOR DTB")


def run(*args: str) -> None:
    print("+", " ".join(args), flush=True)
    subprocess.run(args, check=True)


def relay(args: argparse.Namespace, action: str) -> None:
    run(
        str(args.relay_script),
        str(args.relay_channel),
        action,
        "--device",
        args.relay_device,
    )


class Console:
    def __init__(self, device: str, log_path: Path | None):
        self.port = serial.Serial(
            device,
            baudrate=115200,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0,
            exclusive=True,
        )
        self.log = log_path.open("wb") if log_path else None
        self.buffer = bytearray()

    def close(self) -> None:
        if self.log:
            self.log.close()
        self.port.close()

    def read(self) -> bytes:
        data = self.port.read(self.port.in_waiting or 1)
        if not data:
            return b""
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        if self.log:
            self.log.write(data)
            self.log.flush()
        self.buffer.extend(data)
        del self.buffer[:-65536]
        return data

    def write_line(self, line: str = "") -> None:
        self.port.write(line.encode() + b"\r")
        self.port.flush()


def setup_network(args: argparse.Namespace) -> None:
    # eno1-192-168-1 is intentionally not bound to an interface in its
    # NetworkManager profile, so always specify both interface names.
    run("nmcli", "connection", "up", args.lan_connection, "ifname", args.lan_interface)
    run("nmcli", "connection", "up", args.tftp_connection, "ifname", args.tftp_interface)


def stop_autoboot(console: Console, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        console.read()
        lower = bytes(console.buffer).lower()
        if any(marker in lower for marker in AUTOBOOT_MARKERS):
            console.port.write(b"\r")
            console.port.flush()
            time.sleep(0.5)
            console.write_line()
            time.sleep(0.5)
            return
        time.sleep(0.01)
    raise TimeoutError(f"U-Boot autoboot prompt not seen within {timeout:g} seconds")


def login_and_run(console: Console, args: argparse.Namespace, deadline: float | None) -> int:
    state = "login"
    command_sent = False
    scan_from = 0

    while deadline is None or time.monotonic() < deadline:
        data = console.read()
        if not data:
            time.sleep(0.01)
            continue

        view = bytes(console.buffer)
        tail = view[max(0, scan_from - 256):]
        lower_tail = tail.lower()
        check_boot_output(tail)
        if any(marker in lower_tail for marker in BOOT_FAILURES):
            raise RuntimeError("U-Boot failed to download or validate the FIT image")
        if state == "login" and b"login:" in lower_tail:
            console.write_line(args.username)
            state = "password"
            print("\n+ serial: sent login name", flush=True)
            scan_from = len(view)
        elif state == "password" and b"password:" in lower_tail:
            console.write_line(args.password)
            state = "shell"
            print("\n+ serial: sent login password", flush=True)
            scan_from = len(view)
        elif (
            state == "shell" or (state == "login" and args.monitor_only)
        ) and SHELL_RE.search(tail):
            console.write_line(
                f"{args.command}; __bpi_rc=$?; "
                'echo __BPI_RV2_COMMAND_RC__=$__bpi_rc'
            )
            state = "result"
            command_sent = True
            print("\n+ serial: sent validation command", flush=True)
            scan_from = len(view)
        elif state == "result":
            match = RC_RE.search(tail)
            if match:
                return int(match.group(1))

    if not command_sent:
        raise TimeoutError("Alpine login or shell prompt was not seen before timeout")
    raise TimeoutError("The validation command did not finish before timeout")


def monitor(console: Console, timeout: float) -> None:
    deadline = None if timeout == 0 else time.monotonic() + timeout
    while deadline is None or time.monotonic() < deadline:
        if not console.read():
            time.sleep(0.01)
            continue
        check_boot_output(bytes(console.buffer))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default="/dev/ttyACM0")
    parser.add_argument(
        "--relay-script",
        type=Path,
        default=Path.home() / "src/device_specific_stuff/relay.py",
    )
    parser.add_argument("--relay-device", default="/dev/ttyUSB0")
    parser.add_argument("--relay-channel", type=int, default=1)
    parser.add_argument("--tftp-connection", default="enp4s0-shared")
    parser.add_argument("--tftp-interface", default="enp4s0")
    parser.add_argument("--lan-connection", default="eno1-192-168-1")
    parser.add_argument("--lan-interface", default="eno1")
    parser.add_argument("--board-ip", default="10.42.0.2")
    parser.add_argument("--server-ip", default="10.42.0.1")
    parser.add_argument("--tftp-port", type=int, default=69)
    parser.add_argument("--image", default="kernel.itb")
    parser.add_argument("--uboot-timeout", type=float, default=30)
    parser.add_argument(
        "--timeout",
        type=float,
        default=120,
        help="seconds to stream after bootm; 0 means forever (default: 120)",
    )
    parser.add_argument("--log", type=Path, help="also save raw serial output here")
    parser.add_argument("--skip-network", action="store_true")
    parser.add_argument(
        "--monitor-only",
        action="store_true",
        help="only read the current serial console; do not power-cycle or boot",
    )
    parser.add_argument("--username", default="root")
    parser.add_argument("--password", default="123456")
    parser.add_argument("--command", help="log in and run this command after boot")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.timeout < 0 or args.uboot_timeout <= 0:
        raise SystemExit("timeouts must be non-negative, and --uboot-timeout must be positive")

    if not args.monitor_only and not args.skip_network:
        setup_network(args)

    if not args.monitor_only:
        tftp_file = Path("/var/lib/tftpboot") / args.image
        if not tftp_file.is_file():
            raise SystemExit(f"TFTP image does not exist: {tftp_file}")
        relay(args, "off")
        time.sleep(1)

    console = Console(args.serial, args.log)
    try:
        console.port.reset_input_buffer()
        if args.monitor_only:
            if args.command:
                console.write_line()
        else:
            relay(args, "on")
            stop_autoboot(console, args.uboot_timeout)
            port_command = (
                f"setenv tftpdstp {args.tftp_port}; " if args.tftp_port != 69 else ""
            )
            command = (
                f"setenv ipaddr {args.board_ip}; "
                f"setenv serverip {args.server_ip}; "
                f"{port_command}"
                f"tftpboot {args.image}; bootm"
            )
            print(f"\n+ U-Boot: {command}", flush=True)
            console.write_line(command)

        deadline = None if args.timeout == 0 else time.monotonic() + args.timeout
        if args.command:
            return login_and_run(console, args, deadline)
        monitor(console, args.timeout)
        return 0
    finally:
        console.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        OSError,
        RuntimeError,
        subprocess.CalledProcessError,
        serial.SerialException,
        TimeoutError,
    ) as exc:
        print(f"\nerror: {exc}", file=sys.stderr)
        raise SystemExit(1)
