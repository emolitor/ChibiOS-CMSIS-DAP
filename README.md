# ChibiOS Probe (CMSIS-DAP)

A CMSIS-DAP v2 debug probe for the RP2040 (Raspberry Pi Pico) and RP2350 (Raspberry Pi Pico 2), built on ChibiOS RTOS with dual-core SMP. Implements a USB composite device with a DAP debugger and UART bridge, compatible with the [Raspberry Pi Debug Probe](https://www.raspberrypi.com/products/debug-probe/) pinout.

## Features

- **CMSIS-DAP v2** over USB bulk endpoints (WinUSB — driverless on Windows)
- **UART bridge** via USB CDC ACM with host-selectable baud rate and framing
- **Dual-core SMP model**:
  Core 0 runs the main thread, `DapThread`, and `UartThread`; Core 1 runs `DapProcessThread`
- **PIO-based SWD** Derived from the [Raspberry Pi Debug Probe](https://github.com/raspberrypi/debugprobe)
- **Current ChibiOS GitHub master targets**: RP2040 (Cortex-M0+),
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
- `git` (to check out ChibiOS)
- Python 3, a native C compiler, and `pytest` (for host tests)
- PyUSB and OpenOCD (for the hardware functional tests)

### Build

```bash
make chibios                  # clone/update chibios-upstream/chibios master
make                          # build all targets
make TARGET=rp2040            # build RP2040 only
make TARGET=rp2350            # build RP2350 ARM only
make TARGET=rp2350_riscv      # build RP2350 Hazard3 only
```

`make chibios` tracks GitHub master and refuses to update a dirty or
unexpected checkout. It also applies the compatibility patches in `patches/`;
the current patch makes RP2040/RP2350 core-1 reset follow the boot-ROM FIFO
protocol used by the latest Pico SDK. Each successful update prints the exact
ChibiOS commit; `make chibios-sha` prints it again for test reports. Firmware
builds fail early if the required compatibility is absent.

To use an existing checkout, set `CHIBIOS=/path/to/chibios`. The repository
URL and branch can be overridden with `CHIBIOS_GIT=` and `CHIBIOS_BRANCH=`.
For a reproducible build, pin an upstream revision with `CHIBIOS_REV=`;
`make chibios CHIBIOS_REV=<rev>` checks that revision out instead of tracking
the branch head. Prefer a full immutable commit SHA — tags can be
force-moved, so they are only a convenience; if you pin a tag, record the
commit that `make chibios-sha` reports.

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

The UART test matches the Raspberry Pi Debug Probe topology: each probe's
UART1 on GPIO4/5 is wired to the opposite target's UART0 on GPIO1/0. It loads
a temporary UART0 echo program into target RAM through SWD, tests both probes
at 115200 8N1, 230400 7E2, and 1 Mbaud 8N1, then watchdog-reboots each target
back into the firmware in flash.

### Flash

1. Hold BOOTSEL on the Pico and plug it in
2. Build the UF2: `make TARGET=<target> build/<target>/ch.uf2`
3. Copy the UF2 to the drive — the Pico reboots automatically

Or flash via SWD with OpenOCD / another debug probe.

#### Booting over SWD (dual-core)

The firmware is dual-core (Core 0 runs the USB/DAP/UART threads, Core 1 runs
`DapProcessThread`), so it must reach the USB host only after Core 1 has
launched. A plain `reset run` does **not** reliably start Core 1 — Core 0 ends
up in the idle thread while Core 1 stays parked in the boot ROM, and USB never
enumerates. Boot it one of two ways:

- **Full boot-ROM cold reboot** (recommended for this firmware): trigger a
  watchdog reset so the boot ROM re-launches both cores from flash. The exact
  register sequence per chip is in `tests/functional/uart_link_test.py`
  (`reset_target()` / the `TARGETS` table) — note the RP2040 and RP2350 use
  different `psm`/`wdsel`/`watchdog` values. A transient "Failed to write
  memory" as the chip resets mid-write is expected.
- **Ordered resume** (works for plain demos): `reset halt`, resume Core 1 so it
  reaches the boot-ROM wait-for-vector loop, then resume Core 0 so its FIFO
  handshake launches Core 1.

For the **RP2350 in RISC-V mode**, flash with the `rp2350-auto` OpenOCD target:
`rescue` → `program build/rp2350_riscv/ch.elf verify` → `reset run`. The boot
ROM reads the PICOBIN image block and switches ARM→RISC-V on reset (the ARM
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
