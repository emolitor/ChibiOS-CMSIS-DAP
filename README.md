# ChibiOS Probe (CMSIS-DAP)

A CMSIS-DAP v2 debug probe for the RP2040 (Raspberry Pi Pico) and RP2350 (Raspberry Pi Pico 2), built on ChibiOS RTOS with dual-core SMP. Implements a USB composite device with a DAP debugger and UART bridge, compatible with the [Raspberry Pi Debug Probe](https://www.raspberrypi.com/products/debug-probe/) pinout.

## Features

- **CMSIS-DAP v2** over USB bulk endpoints (WinUSB — driverless on Windows)
- **UART bridge** via USB CDC ACM at 115200 baud
- **RTT (Real-Time Transfer)** via USB CDC — streams SEGGER RTT channel 0 from the target without halting it
- **Dual-core SMP**: Core 0 handles USB, Core 1 processes DAP commands
- **PIO-based SWD** Derived from the [Raspberry Pi Debug Probe](https://github.com/raspberrypi/debugprobe)
- **Dual-target support**: RP2040 (Cortex-M0+) and RP2350 (Cortex-M33)
- **LED status indicator**: off (idle), solid (DAP connected), slow blink (DAP running)

## Performance

Performance is comparable to the Retail Raspberry Pi Debug Probe.

### SWD Clock Speed

| Probe | System Clock | PIO Cycles/Bit | Theoretical Max | Tested Max |
|-------|-------------|-----------------|-----------------|------------|
| RP2040 | 200 MHz | 4 | 50 MHz | 28 MHz |
| RP2350 | 150 MHz | 4 | 37.5 MHz | 25 MHz |

The maximum tested speed is limited by the target's SWD debug port, not the probe's PIO.

### Throughput (64 KB SRAM read via OpenOCD, 3-run average)

| SWD Clock | RP2040 Probe |
|----------:|-------------:|
| 1 MHz | 59 KiB/s |
| 4 MHz | 178 KiB/s |
| 10 MHz | 285 KiB/s |
| 15 MHz | 328 KiB/s |
| 20 MHz | 353 KiB/s |
| 28 MHz | 375 KiB/s |

Throughput plateaus above ~15 MHz as USB bulk transfer overhead becomes the bottleneck. RTT adds zero measurable overhead to DAP throughput (all speeds within ±0.3%).

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
- **Interfaces 1-2**: CDC ACM "UART Bridge" (Bulk EP2 IN/OUT, Interrupt EP3 IN)
- **Interfaces 3-4**: CDC ACM "RTT Console" (Bulk EP4 IN/OUT, Interrupt EP5 IN)

Includes a BOS descriptor with Platform Capability for automatic WinUSB driver binding on Windows.

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

1. Hold BOOTSEL on the Pico and plug it in — it mounts as a USB mass storage drive (e.g., `RPI-RP2`)
2. Build the UF2: `make`
3. Copy `build/rp2040/ch.uf2` or `build/rp2350/ch.uf2` to the drive — the Pico reboots automatically

## License

This project is licensed under the GNU General Public License v2.0 or later — see [LICENSE](LICENSE) for details.

ChibiOS (checked out into `ChibiOS/`) is licensed separately
