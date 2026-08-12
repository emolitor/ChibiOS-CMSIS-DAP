# ChibiOS Probe (CMSIS-DAP)

A CMSIS-DAP v2 debug probe for the RP2040 (Raspberry Pi Pico) and RP2350 (Raspberry Pi Pico 2), built on ChibiOS RTOS with dual-core SMP. Implements a USB composite device with a DAP debugger and UART bridge, compatible with the [Raspberry Pi Debug Probe](https://www.raspberrypi.com/products/debug-probe/) pinout.

## Features

- **CMSIS-DAP v2** over USB bulk endpoints (WinUSB — driverless on Windows)
- **UART bridge** via USB CDC ACM with host-selectable baud rate and framing
- **Dual-core SMP model**:
  Core 0 runs the main thread, `DapThread`, and `UartThread`; Core 1 runs `DapProcessThread`
- **PIO-based SWD** Derived from the [Raspberry Pi Debug Probe](https://github.com/raspberrypi/debugprobe)
- **Validated ChibiOS GitHub master targets**: RP2040 (Cortex-M0+),
  RP2350 (Cortex-M33), and RP2350 (Hazard3 RISC-V)
- **LED status indicator**: off (idle), solid (DAP connected), slow blink (DAP running)

## Performance

Performance is comparable to the Retail Raspberry Pi Debug Probe.

### SWD Clock Speed

| Probe | System Clock | PIO Cycles/Bit | Theoretical Max | Tested Max |
|-------|-------------|-----------------|-----------------|------------|
| RP2040 | 200 MHz | 4 | 50 MHz | 25 MHz |
| RP2350 (ARM) | 150 MHz | 4 | 37.5 MHz | 25 MHz |
The maximum tested speed is limited by the target's SWD debug port, not the probe's PIO.

### Throughput (64 KB SRAM read via OpenOCD)

| Probe | SWD Clock | Throughput |
|-------|-----------|------------|
| RP2350 (ARM) | 15 MHz | 710 KB/s |
| RP2350 (ARM) | 25 MHz | 744 KB/s |
Current Trunk-supported targets are USB-limited at ~745 KB/s at 25 MHz.

## Pin Assignment

| GPIO | Function |
|------|----------|
| 1 | nRESET (open-drain) |
| 2 | SWCLK |
| 3 | SWDIO |
| 4 | UART TX (to target) |
| 5 | UART RX (from target) |
| 25 | LED |

These match the Raspberry Pi Debug Probe pinout, so any wiring guide for that probe applies here.

## USB Device

- **VID**: `0x2E8A` (Raspberry Pi)
- **PID**: `0x000C` (Debug Probe)
- **Interface 0**: CMSIS-DAP v2 (Vendor class, Bulk EP1 IN/OUT)
- **Interfaces 1-2**: CDC ACM UART bridge (Bulk EP2 IN/OUT, Interrupt EP3 IN)

The UART bridge applies the host CDC line-coding settings to UART1, so baud
rate, data bits, parity, and stop bits follow the terminal or debugger
configuration rather than being fixed in firmware.

Includes a BOS descriptor with Platform Capability for automatic WinUSB driver binding on Windows.

## Building

### Prerequisites

- `arm-none-eabi-gcc` toolchain
- `riscv-none-elf-gcc` toolchain (for RP2350 Hazard3)
- `picotool` (for UF2 conversion and flashing)
- `pioasm` 2.2.0 (only when modifying `probe_swd.pio`)
- `git` (to check out ChibiOS)
- Python 3, a native C compiler, and `pytest` (for host tests)
- PyUSB and OpenOCD (for the hardware functional tests)

### Build

```bash
make chibios                  # clone/update the pinned ChibiOS revision
make                          # build all targets
make TARGET=rp2040            # build RP2040 only
make TARGET=rp2350            # build RP2350 ARM only
make TARGET=rp2350_riscv      # build RP2350 Hazard3 only
```

`make chibios` checks out the audited GitHub master commit configured by
`CHIBIOS_REV` and refuses to update a dirty or unexpected checkout. No local
ChibiOS patches are required. Each successful update prints the exact ChibiOS
commit; `make chibios-sha` prints it again for test reports. Firmware builds
fail early if the checkout is dirty or is not at the configured revision.

To use an existing checkout, set `CHIBIOS=/path/to/chibios`. Override the pin
with `CHIBIOS_REV=<rev>`, or pass `CHIBIOS_REV=` explicitly to test the current
`CHIBIOS_BRANCH` head. The repository URL and branch can be overridden with
`CHIBIOS_GIT=` and `CHIBIOS_BRANCH=`. Prefer a full immutable commit SHA — tags
can be force-moved, so they are only a convenience; if you use a tag, record
the commit that `make chibios-sha` reports.

### Tests

```bash
make test                     # native unit/sanitizer and descriptor tests
make check                    # host tests plus all three firmware builds
```

The host suite uses SWD/platform mocks and runs the CMSIS-DAP parser under
AddressSanitizer and UndefinedBehaviorSanitizer. Hardware scripts under
`tests/functional/` validate USB descriptors, CMSIS-DAP commands and atomic
queuing, the CDC/UART bridge, and OpenOCD SWD transfers:

```bash
python3 tests/functional/probe_test.py --serial <probe-serial>
python3 tests/functional/uart_link_test.py \
  --serial-a <rp2040-serial> --serial-b <rp2350-serial>
python3 tests/functional/openocd_probe_test.py \
  --serial <probe-serial> --target rp2040
```

The functional tests require PyUSB, OpenOCD, and physical wiring appropriate
to the requested path. Always select devices by serial number when multiple
probes share VID:PID `2e8a:000c`.

`--target` selects the chip and architecture under debug: `rp2040`, `rp2350`
for an Arm image, or `rp2350_riscv` for a Hazard3 one. Both RP2350 values
share OpenOCD's `target/rp2350.cfg`, which defaults to the Cortex-M pair, so
the RISC-V value additionally selects the `rv0` core. Pointing the Arm value
at a RISC-V image leaves both Cortex-M cores reporting `become unavailable`
and OpenOCD failing with `Target not examined yet`, which resembles a probe
fault but is a target-selection mismatch.

The UART test matches the Raspberry Pi Debug Probe topology: each probe's
UART1 on GPIO4/5 is wired to the opposite target's UART0 on GPIO1/0. It loads
a temporary UART0 echo program into target RAM through SWD, tests both probes
at 115200 8N1, 230400 7E2, and 1 Mbaud 8N1, then watchdog-reboots each target
back into the firmware in flash. Omit `--serial-b` when the far end is a
plain target rather than a second probe; that tests the A-to-B direction only
and skips the re-enumeration wait, which has nothing to wait for.

### Flash

1. Hold BOOTSEL on the Pico and plug it in
2. Build the UF2: `make TARGET=<target> build/<target>/ch.uf2`
3. Copy the UF2 to the drive — the Pico reboots automatically

Or flash via SWD with OpenOCD / another debug probe.

#### Booting over SWD (dual-core)

The firmware is dual-core (Core 0 runs the USB/DAP/UART threads, Core 1 runs
`DapProcessThread`). Let ChibiOS own the secondary-core reset and boot-ROM FIFO
launch by configuring OpenOCD for core 0 only, before loading the target file:

```text
# RP2040
-c "set USE_CORE 0"  -f target/rp2040.cfg

# RP2350 Arm
-c "set USE_CORE cm0" -f target/rp2350.cfg
```

With that setting, `program <elf> verify reset exit` and repeated `reset run`
both work. OpenOCD's default dual-core target configuration also manages core
1 during reset and can conflict with ChibiOS's force-reset/launch sequence; the
result may enumerate on USB but stop answering CMSIS-DAP requests. The legacy
FIFO-notification compatibility patch does not correct that debugger-induced
state.

A full boot-ROM cold reboot is also valid. The watchdog register sequence for
each chip is in `tests/functional/uart_link_test.py` (`reset_target()` and the
`TARGETS` table). A transient "Failed to write memory" as the chip resets
mid-write is expected.

For the **RP2350 in RISC-V mode**, flash with the `rp2350-auto` OpenOCD target:
`rescue`, then `program build/rp2350_riscv/ch.elf verify`, then `reset run`. The
boot ROM reads the PICOBIN image block and switches ARM→RISC-V on reset (the ARM
core then reports `unavailable` and the RISC-V core `running`). Do **not** use
raw watchdog-register writes to drive that transition — it can leave the debug
port wedged.

If an RP2350 debug port becomes unresponsive (cores read `unknown` / "target
not examined", no USB, not even BOOTSEL): lower the SWD clock (e.g.
`adapter speed 200`), run `rescue`, `arp_examine` the ARM core explicitly, then
`reset run`. The slow clock plus explicit examine gets the DAP responding
again.

## License

This project is licensed under the GNU General Public License v2.0 or later — see [LICENSE](LICENSE) for details.

ChibiOS (checked out into `ChibiOS/`) is licensed separately
