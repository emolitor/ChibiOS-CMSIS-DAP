# ChibiOS Probe (CMSIS-DAP)

A CMSIS-DAP v2 debug probe for the RP2040 (Raspberry Pi Pico), built on ChibiOS RTOS with dual-core SMP. Implements a USB composite device with a DAP debugger and UART bridge, compatible with the [Raspberry Pi Debug Probe](https://www.raspberrypi.com/products/debug-probe/) pinout.

## Features

- **CMSIS-DAP v2** over USB bulk endpoints (WinUSB — driverless on Windows)
- **UART bridge** via USB CDC ACM at 115200 baud
- **Dual-core SMP**: Core 0 handles USB, Core 1 processes DAP commands
- **SWD bit-banging** via RP2040 SIO registers for single-cycle GPIO access
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
make                          # build (produces build/ch.elf)
```

### Flash

1. Hold BOOTSEL on the Pico and plug it in (or replug) — it mounts as a USB mass storage drive (e.g., `RPI-RP2`)
2. Build the UF2: `make ch.uf2`
3. Copy `build/ch.uf2` to the drive — the Pico reboots automatically

## License

This project is licensed under the GNU General Public License v2.0 or later — see [LICENSE](LICENSE) for details.

ChibiOS (checked out into `ChibiOS/`) is licensed separately
