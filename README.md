# ChibiOS Probe (CMSIS-DAP)

A CMSIS-DAP v2 debug probe for the RP2040 (Raspberry Pi Pico) and RP2350 (Raspberry Pi Pico 2), built on ChibiOS RTOS with dual-core SMP. Implements a USB composite device with a DAP debugger and UART bridge, compatible with the [Raspberry Pi Debug Probe](https://www.raspberrypi.com/products/debug-probe/) pinout.

## Features

- **CMSIS-DAP v2** over USB bulk endpoints (WinUSB — driverless on Windows)
- **UART bridge** via USB CDC ACM at 115200 baud
- **Dual-core SMP**: Core 0 handles USB, Core 1 processes DAP commands
- **PIO-based SWD** using a hardware state machine for deterministic timing (derived from the [Raspberry Pi Debug Probe](https://github.com/raspberrypi/debugprobe))
- **Dual-target support**: RP2040 (Cortex-M0+) and RP2350 (Cortex-M33)
- **Unique serial number** read from flash chip at boot
- **LED status indicator**: off (idle), solid (DAP connected), slow blink (DAP running)

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

Includes a BOS descriptor with MS OS 2.0 Platform Capability for automatic WinUSB driver binding on Windows.

## Building

### Prerequisites

- `arm-none-eabi-gcc` toolchain
- `picotool` (for UF2 conversion and flashing)
- `svn` (to check out ChibiOS)

### Build

```bash
make chibios                  # check out ChibiOS from SVN (first time only)
make                          # build both targets (produces build/rp2040/ch.elf and build/rp2350/ch.elf)
```

### Flash

1. Hold BOOTSEL on the Pico and plug it in (or replug) — it mounts as a USB mass storage drive (e.g., `RPI-RP2`)
2. Build the UF2: `make`
3. Copy `build/rp2040/ch.uf2` or `build/rp2350/ch.uf2` to the drive — the Pico reboots automatically

## License

This project is licensed under the GNU General Public License v2.0 or later — see [LICENSE](LICENSE) for details.

ChibiOS (checked out into `ChibiOS/`) is licensed separately
