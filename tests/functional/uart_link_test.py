#!/usr/bin/env python3
"""Test each probe's CDC/UART1 bridge against the other board's UART0."""

from __future__ import annotations

import argparse
import pathlib
import struct
import subprocess
import threading
import time

import usb.core
import usb.util


VID = 0x2E8A
PID = 0x000C
CDC_OUT = 0x02
CDC_IN = 0x82
CDC_CONTROL_INTERFACE = 1
ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / "tests" / "build" / "uart_echo"
ECHO_SOURCE = pathlib.Path(__file__).with_name("uart_echo_target.c")
OPENOCD_SCRIPTS = pathlib.Path("/usr/local/share/openocd/scripts")

TARGETS = {
    "rp2040": {
        "cpu": "cortex-m0plus",
        "core": "rp2040.core0",
        "clock": 200_000_000,
        "uart": 0x40034000,
        "io": 0x40014000,
        "pads": 0x4001C000,
        "resets": 0x4000C000,
        "reset_uart0": 1 << 22,
        "psm": 0x40010000,
        "wdsel": 0x0001FFFC,
        "watchdog": 0x40058000,
    },
    "rp2350": {
        "cpu": "cortex-m33",
        "core": "rp2350.cm0",
        "clock": 150_000_000,
        "uart": 0x40070000,
        "io": 0x40028000,
        "pads": 0x40038000,
        "resets": 0x40020000,
        "reset_uart0": 1 << 26,
        "psm": 0x40018000,
        "wdsel": 0x01FFFFF3,
        "watchdog": 0x400D8000,
    },
}


class UartPort:
    def __init__(self, serial: str):
        self.serial = serial
        devices = usb.core.find(find_all=True, idVendor=VID, idProduct=PID)
        matches = []
        for dev in devices:
            try:
                if usb.util.get_string(dev, dev.iSerialNumber) == serial:
                    matches.append(dev)
            except (ValueError, usb.core.USBError):
                # Other attached probes may be resetting or debugger-held.
                continue
        if len(matches) != 1:
            raise RuntimeError(f"unable to uniquely find UART device {serial}")
        self.dev = matches[0]
        self.detached = []
        self.claimed = []
        try:
            try:
                self.dev.get_active_configuration()
            except usb.core.USBError:
                self.dev.set_configuration()
            for interface in (1, 2):
                if self.dev.is_kernel_driver_active(interface):
                    self.dev.detach_kernel_driver(interface)
                    self.detached.append(interface)
                usb.util.claim_interface(self.dev, interface)
                self.claimed.append(interface)
        except Exception:
            # A partially initialized port cannot be closed by the caller, so
            # release claims and reattach kernel drivers before propagating —
            # otherwise a failed retry leaves the CDC device unusable.
            self._release()
            raise

    def configure(self, baud: int, stop_bits: int, parity: int, data_bits: int) -> None:
        line_coding = struct.pack("<IBBB", baud, stop_bits, parity, data_bits)
        assert (
            self.dev.ctrl_transfer(
                0x21,
                0x20,
                0,
                CDC_CONTROL_INTERFACE,
                line_coding,
                timeout=2000,
            )
            == len(line_coding)
        )
        self.dev.ctrl_transfer(
            0x21, 0x22, 1, CDC_CONTROL_INTERFACE, None, timeout=2000
        )
        time.sleep(0.05)

    def write(self, payload: bytes) -> None:
        offset = 0
        while offset < len(payload):
            offset += self.dev.write(CDC_OUT, payload[offset:], timeout=5000)

    def drain(self) -> None:
        while True:
            try:
                self.dev.read(CDC_IN, 256, timeout=50)
            except usb.core.USBTimeoutError:
                return

    def read_exact(self, length: int) -> bytes:
        result = bytearray()
        deadline = time.monotonic() + 3.0
        while len(result) < length and time.monotonic() < deadline:
            try:
                result.extend(
                    self.dev.read(
                        CDC_IN, min(256, length - len(result)), timeout=500
                    )
                )
            except usb.core.USBTimeoutError:
                pass
        if len(result) != length:
            raise AssertionError(f"expected {length} bytes, received {len(result)}")
        return bytes(result)

    def _release(self) -> None:
        # Best effort: a failure restoring one interface must not prevent
        # restoring the rest or disposing the device handle.
        try:
            for interface in reversed(self.claimed):
                try:
                    usb.util.release_interface(self.dev, interface)
                except usb.core.USBError:
                    pass
            self.claimed = []
            for interface in self.detached:
                try:
                    self.dev.attach_kernel_driver(interface)
                except (usb.core.USBError, NotImplementedError):
                    pass
            self.detached = []
        finally:
            usb.util.dispose_resources(self.dev)

    def close(self) -> None:
        self._release()


def uart_dividers(clock: int, baud: int) -> tuple[int, int]:
    divisor = (8 * clock) // baud
    integer = divisor >> 7
    fractional = ((divisor & 0x7F) + 1) // 2
    if fractional >= 64:
        integer += 1
        fractional = 0
    if not 1 <= integer <= 0xFFFF:
        raise ValueError(f"{baud} baud is not representable")
    return integer, fractional


def line_control(stop: int, parity: int, bits: int) -> int:
    value = 1 << 4  # FIFO enable
    value |= (bits - 5) << 5
    if stop == 2:
        value |= 1 << 3
    if parity == 1:
        value |= 1 << 1
    elif parity == 2:
        value |= (1 << 1) | (1 << 2)
    return value


def build_echo(target: str, baud: int, stop: int, parity: int, bits: int) -> pathlib.Path:
    cfg = TARGETS[target]
    integer, fractional = uart_dividers(cfg["clock"], baud)
    output = BUILD / f"{target}-{baud}-{stop}-{parity}-{bits}.elf"
    BUILD.mkdir(parents=True, exist_ok=True)
    command = [
        "arm-none-eabi-gcc",
        f"-mcpu={cfg['cpu']}",
        "-mthumb",
        "-Os",
        "-ffreestanding",
        "-fno-builtin",
        "-nostdlib",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wl,--section-start=.text=0x20010000",
        "-Wl,-e,uart_echo_entry",
        f"-DUART_BASE={cfg['uart']}",
        f"-DIO_BANK0_BASE={cfg['io']}",
        f"-DPADS_BANK0_BASE={cfg['pads']}",
        f"-DRESETS_BASE={cfg['resets']}",
        f"-DRESET_UART0={cfg['reset_uart0']}",
        f"-DUART_IBRD={integer}",
        f"-DUART_FBRD={fractional}",
        f"-DUART_LCR_H={line_control(stop, parity, bits)}",
        str(ECHO_SOURCE),
        "-o",
        str(output),
    ]
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    if completed.returncode != 0:
        raise RuntimeError(completed.stdout + completed.stderr)
    return output


def openocd(serial: str, target: str, commands: list[str]) -> subprocess.CompletedProcess:
    command = [
        "openocd",
        "-s",
        str(OPENOCD_SCRIPTS),
        "-f",
        "interface/cmsis-dap.cfg",
        "-c",
        f"adapter serial {serial}",
        "-c",
        "adapter speed 1000",
        "-f",
        f"target/{target}.cfg",
    ]
    for item in commands:
        command.extend(("-c", item))
    return subprocess.run(
        command, cwd=ROOT, text=True, capture_output=True, timeout=45
    )


def start_echo(serial: str, target: str, image: pathlib.Path) -> None:
    cfg = TARGETS[target]
    completed = openocd(
        serial,
        target,
        [
            "init",
            "halt",
            "sleep 100",
            f"load_image {image}",
            f"verify_image {image}",
            f"targets {cfg['core']}",
            # Mask interrupts before resuming: the flashed firmware's NVIC and
            # SysTick are still configured, so a pending RTOS tick could enter
            # its handlers and context-switch away from the RAM echo program.
            "reg primask 1",
            "resume 0x20010000",
            "shutdown",
        ],
    )
    if completed.returncode != 0:
        raise RuntimeError(completed.stdout + completed.stderr)
    time.sleep(0.1)


def reset_target(serial: str, target: str) -> None:
    cfg = TARGETS[target]
    completed = openocd(
        serial,
        target,
        [
            "init",
            f"mww 0x{cfg['psm'] + 8:08x} 0x{cfg['wdsel']:08x}",
            f"mww 0x{cfg['watchdog'] + 0x1C:08x} 0",
            f"mww 0x{cfg['watchdog']:08x} 0x80000000",
            "shutdown",
        ],
    )
    output = completed.stdout + completed.stderr
    if completed.returncode != 0 and "Failed to write memory" not in output:
        raise RuntimeError(output)
    time.sleep(2.0)


def wait_for_uart(serial: str, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            port = UartPort(serial)
            port.close()
            return
        except (RuntimeError, usb.core.USBError) as exc:
            # Right after the watchdog reboot the descriptor may be readable
            # while set_configuration / interface claim still raise a transient
            # USBError; keep retrying within the timeout window.
            last_error = exc
            time.sleep(0.25)
    raise RuntimeError(f"UART device {serial} did not re-enumerate") from last_error


def exchange_full_duplex(port: UartPort, payload: bytes) -> bytes:
    write_error = []

    def write_payload() -> None:
        try:
            port.write(payload)
        except Exception as exc:  # Propagate PyUSB errors to the main thread.
            write_error.append(exc)

    # Daemon so a hung USB write cannot keep the process alive after the join
    # times out below and the function fails fast.
    writer = threading.Thread(target=write_payload, daemon=True)
    writer.start()
    received = port.read_exact(len(payload))
    writer.join(timeout=5.0)
    if writer.is_alive():
        raise AssertionError("USB CDC writer did not finish")
    if write_error:
        raise write_error[0]
    return received


def exercise_probe(source_serial: str, target_serial: str, target: str) -> None:
    failures = []
    try:
        for baud, stop, parity, bits in (
            (115200, 0, 0, 8),  # 8N1
            (230400, 2, 2, 7),  # 7E2
            (1_000_000, 0, 0, 8),
        ):
            image = build_echo(target, baud, stop, parity, bits)
            start_echo(source_serial, target, image)
            port = UartPort(source_serial)
            try:
                port.configure(baud, stop, parity, bits)
                port.drain()
                alphabet = bytes(range(1 << bits))
                payload = alphabet * (2048 // len(alphabet))
                received = exchange_full_duplex(port, payload)
                if received != payload:
                    failures.append(
                        f"{baud} baud/{bits} data bits: received data differs"
                    )
            except AssertionError as exc:
                failures.append(f"{baud} baud/{bits} data bits: {exc}")
            finally:
                port.close()
    finally:
        reset_target(source_serial, target)

    # The reset target must enumerate again before it becomes the probe for
    # the opposite direction.
    wait_for_uart(target_serial)
    if failures:
        raise AssertionError(
            f"UART bridge {source_serial} -> {target} echo failures:\n"
            + "\n".join(failures)
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial-a", required=True)
    parser.add_argument("--serial-b", required=True)
    parser.add_argument("--target-a", choices=TARGETS, default="rp2040")
    parser.add_argument("--target-b", choices=TARGETS, default="rp2350")
    args = parser.parse_args()

    exercise_probe(args.serial_a, args.serial_b, args.target_b)
    exercise_probe(args.serial_b, args.serial_a, args.target_a)
    print("Bidirectional UART1-to-target-UART0 tests passed")


if __name__ == "__main__":
    main()
