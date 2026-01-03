# paravr - Parallel Port AVR Programmer

A simple, dependency-free AVR ISP programmer for Linux using a real parallel port.
Supports both onboard LPT ports and PCI/PCIe parallel port cards.

## Features

- **Zero dependencies** - Uses only Python standard library and Linux ppdev
- **Auto-detection** - Automatically finds the correct configuration for your hardware
- **PCI card support** - Works with add-in parallel port cards (which often have non-standard behavior)
- **Configuration saving** - Saves working config so you don't need to specify options every time
- **Intel HEX support** - Programs standard .hex files from Arduino IDE, avr-gcc, etc.

## Requirements

- Linux with the `ppdev` kernel module
- A **real** parallel port (USB-to-parallel adapters will NOT work)
- Python 3.7+
- Root access (or appropriate permissions for `/dev/parport0`)

## Wiring

Connect your parallel port to the AVR's ISP header:

```
DB-25 Parallel Port          AVR / Arduino Uno
───────────────────          ─────────────────
Pin 2  (D0)           →      MOSI  (Arduino pin 11)
Pin 3  (D1)           →      SCK   (Arduino pin 13)
Pin 4  (D2)           →      RESET (Arduino RESET pin)
Pin 10 (ACK/S6)       ←      MISO  (Arduino pin 12)
Pin 18-25 (GND)       →      GND

      DB-25 Female                ICSP Header
    (directly on cable)          (on Arduino)
                              
    13 12 11 10  9  8  7         MISO ● ○ VCC
     ○  ○  ○  ○  ○  ○  ○          SCK ○ ○ MOSI
                                RESET ○ ○ GND
    25 24 23 22 21 20 19 18
     ○  ○  ○  ○  ○  ○  ○  ○
           (all GND)
     
     1  2  3  4  5  6  7  8
     ○  ●  ●  ●  ○  ○  ○  ○
        │  │  │
        │  │  └── RESET (D2)
        │  └───── SCK   (D1)
        └──────── MOSI  (D0)
```

### Making a Cable

You'll need:
- DB-25 male connector (or female if connecting to extension cable)
- 5 wires (4 signal + ground)
- 2x3 pin header for ICSP, or individual Dupont wires

Connections:
| DB-25 Pin | Signal | AVR Pin | Arduino Uno |
|-----------|--------|---------|-------------|
| 2         | MOSI   | MOSI    | Pin 11      |
| 3         | SCK    | SCK     | Pin 13      |
| 4         | RESET  | RESET   | RESET       |
| 10        | MISO   | MISO    | Pin 12      |
| 18-25     | GND    | GND     | GND         |

## Quick Start

### 1. Load the ppdev module

```bash
sudo modprobe ppdev
```

To load automatically on boot, add to `/etc/modules-load.d/ppdev.conf`:
```
ppdev
```

### 2. Auto-detect your configuration

```bash
sudo python3 paravr.py --detect
```

This tests various configurations and saves the working one.

### 3. Program your AVR

```bash
sudo python3 paravr.py --hex firmware.hex
```

## Usage

### Auto-Detection (First Time Setup)

```bash
sudo python3 paravr.py --detect
```

This will:
1. Test all MISO pin configurations
2. Test various speeds
3. Find and save the working configuration

### Programming

```bash
# Program with auto-detected config
sudo python3 paravr.py --hex firmware.hex

# Program a specific MCU (default is ATmega328P)
sudo python3 paravr.py --hex firmware.hex --mcu m168p

# Skip verification (faster but less safe)
sudo python3 paravr.py --hex firmware.hex --no-verify

# Force program even if signature doesn't match
sudo python3 paravr.py --hex firmware.hex --force
```

### Reading Fuses

```bash
sudo python3 paravr.py --fuses
```

### Testing the Parallel Port

```bash
sudo python3 paravr.py --test
```

This toggles the output pins - watch for the SCK LED (pin 13) blinking on Arduino.

### Manual Configuration

If auto-detect doesn't work, you can specify options manually:

```bash
# Standard onboard parallel port
sudo python3 paravr.py --hex firmware.hex --miso 10

# PCI parallel port card (no STATUS inversion)
sudo python3 paravr.py --hex firmware.hex --miso 100

# With slower speed
sudo python3 paravr.py --hex firmware.hex --miso 100 --speed 500
```

### List Options

```bash
# List supported MCUs
sudo python3 paravr.py --list-mcus

# List MISO configurations  
sudo python3 paravr.py --list-miso
```

## MISO Configurations

Different parallel ports map the MISO input differently:

| ID  | Description | Use When |
|-----|-------------|----------|
| 10  | Pin 10, inverted | Standard onboard LPT ports |
| 100 | Pin 10, not inverted | Most PCI/PCIe parallel cards |
| 11  | Pin 11, inverted | MISO wired to BUSY pin |
| 101 | Pin 11, not inverted | PCI card with MISO on BUSY |

Run `--detect` to automatically find the correct one.

## Supported MCUs

| ID | Name | Flash | EEPROM |
|----|------|-------|--------|
| m328p | ATmega328P | 32K | 1K |
| m328 | ATmega328 | 32K | 1K |
| m168p | ATmega168P | 16K | 512 |
| m168 | ATmega168 | 16K | 512 |
| m88p | ATmega88P | 8K | 512 |
| m88 | ATmega88 | 8K | 512 |
| m48p | ATmega48P | 4K | 256 |
| m48 | ATmega48 | 4K | 256 |
| m2560 | ATmega2560 | 256K | 4K |
| m1280 | ATmega1280 | 128K | 4K |
| m32u4 | ATmega32U4 | 32K | 1K |
| t85 | ATtiny85 | 8K | 512 |
| t45 | ATtiny45 | 4K | 256 |
| t25 | ATtiny25 | 2K | 128 |
| t84 | ATtiny84 | 8K | 512 |
| t44 | ATtiny44 | 4K | 256 |
| t24 | ATtiny24 | 2K | 128 |
| t2313 | ATtiny2313 | 2K | 128 |

## Troubleshooting

### "Permission denied for /dev/parport0"

Run with `sudo`, or add yourself to the `lp` group:
```bash
sudo usermod -a -G lp $USER
# Log out and back in
```

### "/dev/parport0 not found"

Load the ppdev module:
```bash
sudo modprobe ppdev
```

### "Failed to enter programming mode"

1. **Check power** - Is the Arduino/AVR powered? (USB connected)
2. **Check ground** - GND must be connected (DB-25 pins 18-25)
3. **Check wiring** - Verify all 4 signal wires are correct
4. **Try slower speed** - `--speed 500` or `--speed 1000`
5. **Run --detect** - Let it find the correct configuration

### "No working configuration found"

- Verify wiring with a multimeter
- Make sure the AVR has a working clock (external crystal or internal)
- Try pressing the reset button on Arduino while running --detect
- Check that you're using a REAL parallel port (USB adapters don't work)

### PCI Parallel Port Cards

Many PCI/PCIe parallel port cards don't implement hardware inversion on STATUS bits.
Run `--detect` which will try both inverted and non-inverted configurations.

Common cards that work:
- Asix AX99100 based cards
- MosChip MCS9900 based cards
- Most "native" PCIe parallel cards

## Getting .hex Files

### From Arduino IDE

1. Open your sketch
2. Sketch → Export compiled Binary
3. Find the `.hex` file in your sketch folder

### From avr-gcc

```bash
avr-gcc -mmcu=atmega328p -o firmware.elf firmware.c
avr-objcopy -O ihex firmware.elf firmware.hex
```

### From PlatformIO

```bash
pio run
# .hex file is in .pio/build/*/firmware.hex
```

## Configuration File

Auto-detected settings are saved to:
```
~/.config/paravr/config.json
```

Format:
```json
{
  "/dev/parport0": {
    "miso_config": 100,
    "speed_us": 100,
    "invert_reset": false
  }
}
```

## How It Works

This programmer uses "bit-banging" - directly controlling the parallel port's
DATA pins to generate the SPI signals (MOSI, SCK, RESET) and reading the STATUS
register for MISO.

The parallel port is accessed via Linux's `ppdev` interface, which provides
user-space access to the port's registers through ioctl calls.

### Why USB Adapters Don't Work

USB-to-parallel adapters only implement the printer protocol, not direct
register access. They can't bit-bang SPI signals fast enough or read inputs
in real-time.

## License

MIT License - See source file for details.
