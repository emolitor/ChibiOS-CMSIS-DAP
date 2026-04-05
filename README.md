# ChibiOS Probe (CMSIS-DAP)

A CMSIS-DAP v2 debug probe for the RP2040 (Raspberry Pi Pico) and RP2350 (Raspberry Pi Pico 2), built on ChibiOS RTOS with dual-core SMP. Implements a USB composite device with a DAP debugger and UART bridge, compatible with the [Raspberry Pi Debug Probe](https://www.raspberrypi.com/products/debug-probe/) pinout.

## Features

- **CMSIS-DAP v2** over USB bulk endpoints (WinUSB — driverless on Windows)
- **UART bridge** via USB CDC ACM with host-selectable baud rate and framing
- **Dual-core SMP model**:
  Core 0 runs the main thread, `DapThread`, and `UartThread`; Core 1 runs `DapProcessThread`
- **PIO-based SWD** Derived from the [Raspberry Pi Debug Probe](https://github.com/raspberrypi/debugprobe)
- **Current ChibiOS Trunk targets**: RP2040 (Cortex-M0+) and RP2350 (Cortex-M33)
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
- `picotool` (for UF2 conversion and flashing)
- `svn` (to check out ChibiOS)

### Build

```bash
make chibios                  # check out ChibiOS from SVN (first time only)
make                          # build the default ARM targets (rp2040, rp2350)
make TARGET=rp2040            # build RP2040 only
make TARGET=rp2350            # build RP2350 ARM only
```

### Flash

1. Hold BOOTSEL on the Pico and plug it in
2. Build the UF2: `make TARGET=<target> build/<target>/ch.uf2`
3. Copy the UF2 to the drive — the Pico reboots automatically

Or flash via SWD with OpenOCD / another debug probe.

## License

This project is licensed under the GNU General Public License v2.0 or later — see [LICENSE](LICENSE) for details.

ChibiOS (checked out into `ChibiOS/`) is licensed separately
