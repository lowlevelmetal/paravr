# paravr - Parallel Port AVR Programmer

A small, dependency-free AVR ISP programmer for Linux using a real parallel port,
either onboard or a PCI/PCIe card.

- **Auto-detection** finds the MISO pin, signal polarity and fastest working clock
- **Saved settings** so you don't need to pass options every time
- **Target identified by signature**, no need to name the MCU
- **Intel HEX** files from the Arduino IDE, avr-gcc, PlatformIO, etc.

## Requirements

- Linux with the `ppdev` kernel module
- A **real** parallel port. USB-to-parallel adapters only speak the printer
  protocol and can't bit-bang SPI.
- A C++17 compiler and CMake 3.16 or newer
- Root, or membership in the `lp` group

## Building

```bash
cmake -B build
cmake --build build
sudo cmake --install build    # optional, installs paravr to /usr/local/bin
```

The examples below assume paravr is installed; otherwise run `./build/paravr`.
To install elsewhere, configure with `-DCMAKE_INSTALL_PREFIX=...`.

### Tests

```bash
ctest --test-dir build
```

The tests need no hardware. Besides unit tests, they run the `paravr` binary
against a simulated AVR (`tests/avrsim`) that is preloaded in place of
`/dev/parport0`, covering detection, programming, verification and fuses.
Configure with `-DPARAVR_BUILD_TESTS=OFF` to skip them.

## Wiring

| DB-25 Pin | Signal | AVR Pin | Arduino Uno |
|-----------|--------|---------|-------------|
| 2         | MOSI   | MOSI    | Pin 11      |
| 3         | SCK    | SCK     | Pin 13      |
| 4         | RESET  | RESET   | RESET       |
| 10        | MISO   | MISO    | Pin 12      |
| 18-25     | GND    | GND     | GND         |

The AVR's 6-pin ICSP header (as on the Arduino) looks like this from above:

```
MISO  1 ● ○ 2  VCC
SCK   3 ○ ○ 4  MOSI
RESET 5 ○ ○ 6  GND
```

The target must be powered separately (e.g. by USB); the parallel port does not supply VCC.

## Quick Start

```bash
sudo modprobe ppdev              # add "ppdev" to /etc/modules-load.d/ppdev.conf to load at boot
sudo paravr --detect           # find and save working settings
sudo paravr --hex firmware.hex # program
```

With the standard wiring above and a standard port, `--detect` is optional; it
also finds the fastest clock your setup handles.

## Usage

```bash
# Program (erase, write, verify)
sudo paravr --hex firmware.hex

# Skip verification (faster) or erasing
sudo paravr --hex firmware.hex --no-verify
sudo paravr --hex firmware.hex --no-erase

# Program a part that is not in --list-mcus, or whose signature doesn't match
sudo paravr --hex firmware.hex --mcu m328p

# Read fuse and lock bytes
sudo paravr --fuses

# Toggle the output pins (watch the SCK LED on Arduino pin 13) and show the status register
sudo paravr --test

# List supported MCUs
paravr --list-mcus
```

Options given on the command line override the saved settings, e.g.
`--speed 500` to try a slower clock or `--port /dev/parport1` for a second port.

## MISO Pin and Polarity

MISO can be wired to any of the status pins 10, 11, 12, 13 or 15 (`--miso PIN`,
default 10). A standard port inverts BUSY (pin 11) in hardware and no other
status input; paravr accounts for that. If a card deviates from the standard,
add `--invert-miso`. `--detect` tries every pin both ways, and both RESET
polarities (`--invert-reset`, for a RESET line driven through an inverter).

## Supported MCUs

| ID | Name | Flash |
|----|------|-------|
| m328p | ATmega328P | 32K |
| m328 | ATmega328 | 32K |
| m168p | ATmega168P | 16K |
| m168 | ATmega168 | 16K |
| m88p | ATmega88P | 8K |
| m88 | ATmega88 | 8K |
| m48p | ATmega48P | 4K |
| m48 | ATmega48 | 4K |
| m2560 | ATmega2560 | 256K |
| m1280 | ATmega1280 | 128K |
| m32u4 | ATmega32U4 | 32K |
| t85 | ATtiny85 | 8K |
| t45 | ATtiny45 | 4K |
| t25 | ATtiny25 | 2K |
| t84 | ATtiny84 | 8K |
| t44 | ATtiny44 | 4K |
| t24 | ATtiny24 | 2K |
| t2313 | ATtiny2313 | 2K |

## Troubleshooting

**`/dev/parport0: Permission denied`**: run with `sudo`, or add yourself to
the `lp` group (`sudo usermod -a -G lp $USER`, then log in again).

**`/dev/parport0: No such file or directory`**: load the module with `sudo modprobe ppdev`.

**`target not responding`**:

1. Is the target powered, and is GND connected (DB-25 pins 18-25)?
2. Check the four signal wires.
3. Try a slower clock: `--speed 500` or `--speed 1000`.
4. Run `--detect`.

**`No working settings found`**: check the wiring with a multimeter, and make
sure the AVR has a working clock source (a chip fused for an external crystal
needs one).

**`verification failed`**: try a slower `--speed`.

## Getting .hex Files

- **Arduino IDE**: Sketch → Export Compiled Binary; the `.hex` is in the sketch folder.
- **avr-gcc**: `avr-objcopy -O ihex -j .text -j .data firmware.elf firmware.hex`
- **PlatformIO**: `pio run`; the file is `.pio/build/*/firmware.hex`.

## Configuration File

`--detect` saves settings per port in `~/.config/paravr/config` (under `sudo`,
that is root's home):

```
# device miso-pin invert-miso speed-us invert-reset
/dev/parport0 10 0 50 0
```

## How It Works

paravr bit-bangs SPI: it drives MOSI, SCK and RESET on the port's DATA pins and
reads MISO from the STATUS register, using the Linux `ppdev` ioctl interface. On
top of that it speaks the AVR serial programming protocol: Programming Enable,
signature and fuse reads, chip erase, and page-wise flash writes with read-back
verification.

## Project Layout

```
include/paravr/   core library interface
  port.hpp          ppdev parallel port access
  isp.hpp           AVR serial programming protocol
  hex.hpp           Intel HEX parsing
  mcu.hpp           supported MCUs
  settings.hpp      wiring settings and the config file
src/              core library and the command-line tool (main.cpp, commands.cpp)
tests/            unit tests, end-to-end CLI tests and the avrsim target simulator
```

## License

MIT
