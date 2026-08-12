#!/usr/bin/env python3
"""Exercise a flashed probe against a cross-wired RP target using OpenOCD."""

from __future__ import annotations

import argparse
import pathlib
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[2]
ARTIFACTS = ROOT / "tests" / "artifacts"
OPENOCD_SCRIPTS = pathlib.Path("/usr/local/share/openocd/scripts")

# OpenOCD invocation per firmware target. Both RP2350 builds share
# target/rp2350.cfg, which defaults to the Cortex-M pair. On a RISC-V image
# the ROM table presence bits those cores are probed with read zero, so they
# report "become unavailable" and examination never runs; selecting the
# Hazard3 core up front avoids a target-side mismatch that otherwise looks
# like a probe failure.
TARGETS = {
    "rp2040": {"cfg": "target/rp2040.cfg", "use_core": None},
    "rp2350": {"cfg": "target/rp2350.cfg", "use_core": None},
    "rp2350_riscv": {"cfg": "target/rp2350.cfg", "use_core": "rv0"},
}


def openocd_prefix(serial: str, target: str, speed: int) -> list[str]:
    """Adapter and target selection shared by every invocation.

    USE_CORE must be set before the target config is sourced, as the config
    reads it while creating the target objects.
    """
    cfg = TARGETS[target]
    command = [
        "openocd",
        "-s",
        str(OPENOCD_SCRIPTS),
        "-f",
        "interface/cmsis-dap.cfg",
        "-c",
        f"adapter serial {serial}",
        "-c",
        f"adapter speed {speed}",
    ]
    if cfg["use_core"] is not None:
        command.extend(("-c", f"set USE_CORE {cfg['use_core']}"))
    command.extend(("-f", cfg["cfg"]))
    return command


def restore_target(serial: str, target: str, baseline: pathlib.Path) -> None:
    command = openocd_prefix(serial, target, 1000) + [
        "-c",
        "init",
        "-c",
        "halt",
        "-c",
        "sleep 100",
    ]
    if baseline.exists() and baseline.stat().st_size == 0x10000:
        command.extend(("-c", f"load_image {baseline} 0x20010000 bin"))
    command.extend(("-c", "resume", "-c", "shutdown"))

    completed = subprocess.run(
        command, cwd=ROOT, text=True, capture_output=True, timeout=45
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "failed to restore and resume target:\n"
            + completed.stdout
            + completed.stderr
        )


def run_openocd(serial: str, target: str, speed: int, iteration: int) -> None:
    stem = f"{serial}-{target}-{speed}-{iteration}"
    baseline_dump = ARTIFACTS / f"{stem}-baseline.bin"
    modified_dump = ARTIFACTS / f"{stem}-modified.bin"
    scratch = 0x20010000
    baseline_dump.unlink(missing_ok=True)
    modified_dump.unlink(missing_ok=True)

    command = openocd_prefix(serial, target, speed) + [
        "-c",
        "init",
        "-c",
        "halt",
        "-c",
        f"dump_image {baseline_dump} 0x{scratch:08x} 0x10000",
        "-c",
        f"mww 0x{scratch:08x} 0x12345678",
        "-c",
        f"mww 0x{scratch + 4:08x} 0xa5a55a5a",
        "-c",
        f"dump_image {modified_dump} 0x{scratch:08x} 0x10000",
        "-c",
        "shutdown",
    ]
    try:
        completed = subprocess.run(
            command, cwd=ROOT, text=True, capture_output=True, timeout=45
        )
    finally:
        restore_target(serial, target, baseline_dump)

    output = completed.stdout + completed.stderr
    if completed.returncode != 0:
        raise RuntimeError(output)
    if "Examination succeed" not in output:
        raise AssertionError(output)
    baseline = baseline_dump.read_bytes()
    modified = modified_dump.read_bytes()
    assert modified[:4] == bytes((0x78, 0x56, 0x34, 0x12))
    assert modified[4:8] == bytes((0x5A, 0x5A, 0xA5, 0xA5))
    if modified[8:] != baseline[8:]:
        raise AssertionError(f"64 KiB read changed outside scratch words at {speed} kHz")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--target", choices=tuple(TARGETS), required=True)
    args = parser.parse_args()

    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    # 15 MHz is the highest rate that is repeatable on the cross-wired
    # development fixture. Keep the suite below its 25 MHz signal-integrity
    # limit so a cable failure is not mistaken for a firmware regression.
    for speed in (1000, 5000, 15000):
        for iteration in range(2):
            run_openocd(args.serial, args.target, speed, iteration)
    print(
        f"OpenOCD functional tests passed for {args.serial} "
        f"against {args.target}"
    )


if __name__ == "__main__":
    main()
