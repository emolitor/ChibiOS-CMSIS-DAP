#!/usr/bin/env python3
"""USB-level CMSIS-DAP functional tests for a flashed ChibiOS probe."""

from __future__ import annotations

import argparse
import struct
import time

import usb.core
import usb.util


VID = 0x2E8A
PID = 0x000C
DAP_OUT = 0x01
DAP_IN = 0x81
PACKET_SIZE = 64


class Probe:
    def __init__(self, serial: str):
        devices = list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID))
        identified = []
        for dev in devices:
            try:
                identified.append(
                    (
                        dev,
                        usb.util.get_string(dev, dev.iProduct),
                        usb.util.get_string(dev, dev.iSerialNumber),
                    )
                )
            except (ValueError, usb.core.USBError):
                # A second attached probe may be resetting or held by a
                # debugger. It must not prevent selection of the requested
                # device by serial number.
                identified.append((dev, "<unavailable>", "<unavailable>"))

        matches = [dev for dev, _product, found_serial in identified
                   if found_serial == serial]
        if len(matches) != 1:
            found = [(product, found_serial)
                     for _dev, product, found_serial in identified]
            raise RuntimeError(
                f"expected one probe with serial {serial}, "
                f"found {len(matches)}; "
                f"available devices: {found}"
            )
        self.dev = matches[0]
        self.detached = []

        try:
            self.dev.get_active_configuration()
        except usb.core.USBError:
            self.dev.set_configuration()
        for interface in (0,):
            if self.dev.is_kernel_driver_active(interface):
                self.dev.detach_kernel_driver(interface)
                self.detached.append(interface)
            usb.util.claim_interface(self.dev, interface)

    def close(self) -> None:
        for interface in (0,):
            usb.util.release_interface(self.dev, interface)
        for interface in self.detached:
            self.dev.attach_kernel_driver(interface)
        usb.util.dispose_resources(self.dev)

    def write(self, request: bytes) -> None:
        # PyUSB may perform a partial write; loop until the whole request is
        # sent rather than asserting a single full write.
        offset = 0
        while offset < len(request):
            offset += self.dev.write(DAP_OUT, request[offset:], timeout=2000)

    def read(self) -> bytes:
        return bytes(self.dev.read(DAP_IN, PACKET_SIZE, timeout=5000))

    def exchange(self, request: bytes) -> bytes:
        self.write(request)
        return self.read()


def info(probe: Probe, info_id: int) -> bytes:
    response = probe.exchange(bytes((0x00, info_id)))
    assert response[0] == 0x00
    assert len(response) == response[1] + 2
    return response[2:]


def validate_descriptors(probe: Probe, serial: str) -> None:
    dev = probe.dev
    assert dev.bcdUSB == 0x0201
    assert dev.bcdDevice == 0x0220
    assert usb.util.get_string(dev, dev.iManufacturer) == "Raspberry Pi"
    assert usb.util.get_string(dev, dev.iProduct) == "ChibiOS Probe (CMSIS-DAP)"
    assert usb.util.get_string(dev, dev.iSerialNumber) == serial

    configuration = dev.get_active_configuration()
    interfaces = {(intf.bInterfaceNumber, intf.bAlternateSetting) for intf in configuration}
    assert interfaces == {(0, 0), (1, 0), (2, 0)}
    endpoints = {
        ep.bEndpointAddress
        for intf in configuration
        for ep in intf
    }
    assert endpoints == {0x01, 0x81, 0x02, 0x82, 0x83}

    bos = bytes(dev.ctrl_transfer(0x80, 0x06, 0x0F00, 0, 33, timeout=2000))
    assert len(bos) == 33
    assert bos[1] == 0x0F
    assert bos[4] == 1

    ms_os = bytes(dev.ctrl_transfer(0xC0, 0x01, 0, 0x0007, 178, timeout=2000))
    assert len(ms_os) == 178
    assert b"WINUSB" in ms_os


def validate_protocol(probe: Probe) -> None:
    assert info(probe, 0x01) == b"ChibiOS\x00"
    assert info(probe, 0x02) == b"ChibiOS Probe (CMSIS-DAP)\x00"
    assert info(probe, 0x04) == b"2.1.2\x00"
    assert info(probe, 0xFE) == bytes((14,))
    assert info(probe, 0xFF) == struct.pack("<H", PACKET_SIZE)
    capabilities = info(probe, 0xF0)
    assert capabilities[0] & 0x01
    assert capabilities[0] & 0x10
    assert capabilities[1] & 0x01

    # Unsupported and malformed commands use the standard invalid response.
    assert probe.exchange(bytes((0x14,))) == bytes((0xFF,))
    assert probe.exchange(bytes((0x11,))) == bytes((0xFF,))

    execute = bytes(
        (
            0x7F,
            2,
            0x00,
            0xFF,
            0x00,
            0xFE,
        )
    )
    response = probe.exchange(execute)
    assert response == bytes((0x7F, 2, 0x00, 2, 64, 0, 0x00, 1, 14))

    # Queue packets have no immediate response. The next ordinary command
    # commits the batch and produces one ExecuteCommands response per packet.
    probe.write(bytes((0x7E, 1, 0x00, 0xFF)))
    probe.write(bytes((0x7E, 1, 0x00, 0xFE)))
    probe.write(bytes((0x00, 0x04)))
    assert probe.read() == bytes((0x7F, 1, 0x00, 2, 64, 0))
    assert probe.read() == bytes((0x7F, 1, 0x00, 1, 14))
    version = probe.read()
    assert version[0:2] == bytes((0x00, 6))
    assert version[2:] == b"2.1.2\x00"

    assert probe.exchange(bytes((0x02, 0x01))) == bytes((0x02, 0x01))
    assert probe.exchange(bytes((0x11, 0x40, 0x42, 0x0F, 0x00))) == bytes(
        (0x11, 0x00)
    )
    assert probe.exchange(bytes((0x13, 0x00))) == bytes((0x13, 0x00))
    assert probe.exchange(bytes((0x03,))) == bytes((0x03, 0x00))

    for _ in range(25):
        assert probe.exchange(bytes((0x02, 0x01)))[1] == 0x01
        assert probe.exchange(bytes((0x03,))) == bytes((0x03, 0x00))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--reenumeration-delay", type=float, default=0.0)
    args = parser.parse_args()

    if args.reenumeration_delay:
        time.sleep(args.reenumeration_delay)
    probe = Probe(args.serial)
    try:
        validate_descriptors(probe, args.serial)
        validate_protocol(probe)
    finally:
        probe.close()
    print(f"USB CMSIS-DAP functional tests passed for {args.serial}")


if __name__ == "__main__":
    main()
