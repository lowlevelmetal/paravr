#!/usr/bin/env python3
"""
paravr.py - Bit-bang AVR ISP programmer using Linux parallel port (ppdev)

A simple, dependency-free AVR programmer for Linux systems with real parallel ports.
Supports both onboard LPT ports and PCI/PCIe parallel port cards.

Author: Generated with assistance from GitHub Copilot
License: MIT
"""

import argparse
import fcntl
import json
import os
import re
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ============================================================================
# Constants
# ============================================================================

# ppdev ioctl constants (from linux/ppdev.h)
PPCLAIM = 0x708B
PPRELEASE = 0x708C
PPDATADIR = 0x40047090
PPWDATA = 0x40017086
PPRSTATUS = 0x80017081

# Default configuration file location
CONFIG_FILE = Path.home() / ".config" / "paravr" / "config.json"

# ============================================================================
# MISO Pin Configurations
# ============================================================================
# Standard PC parallel port STATUS register bits:
#   Bit 7 (0x80) = pin 11 BUSY   (inverted on standard ports)
#   Bit 6 (0x40) = pin 10 ACK    (inverted on standard ports)
#   Bit 5 (0x20) = pin 12 PE     (not inverted)
#   Bit 4 (0x10) = pin 13 SELECT (not inverted)
#   Bit 3 (0x08) = pin 15 ERROR  (inverted on standard ports)
#
# PCI/PCIe cards may not implement hardware inversion!
#
# Format: config_id: (description, bitmask, is_inverted)

MISO_CONFIGS: Dict[int, Tuple[str, int, bool]] = {
    # Standard configurations (onboard parallel ports)
    10: ("Pin 10 (ACK), inverted - standard LPT", 0x40, True),
    11: ("Pin 11 (BUSY), inverted", 0x80, True),
    12: ("Pin 12 (PE), not inverted", 0x20, False),
    13: ("Pin 13 (SELECT), not inverted", 0x10, False),
    15: ("Pin 15 (ERROR), inverted", 0x08, True),
    # PCI/PCIe card configurations (often no hardware inversion)
    100: ("Pin 10 (ACK), NOT inverted - PCI cards", 0x40, False),
    101: ("Pin 11 (BUSY), NOT inverted - PCI cards", 0x80, False),
    103: ("Pin 15 (ERROR), NOT inverted - PCI cards", 0x08, False),
    # Non-standard bit mappings seen on some cards
    200: ("Bit 2, inverted", 0x04, True),
    201: ("Bit 2, NOT inverted", 0x04, False),
}

# ============================================================================
# Supported MCUs
# ============================================================================
@dataclass
class MCU:
    name: str
    signature: Tuple[int, int, int]
    page_size: int      # bytes
    flash_size: int     # bytes
    eeprom_size: int    # bytes

MCUS: Dict[str, MCU] = {
    # ATmega series
    "m328p":  MCU("ATmega328P",  (0x1E, 0x95, 0x0F), 128, 32768, 1024),
    "m328":   MCU("ATmega328",   (0x1E, 0x95, 0x14), 128, 32768, 1024),
    "m168p":  MCU("ATmega168P",  (0x1E, 0x94, 0x0B), 128, 16384, 512),
    "m168":   MCU("ATmega168",   (0x1E, 0x94, 0x06), 128, 16384, 512),
    "m88p":   MCU("ATmega88P",   (0x1E, 0x93, 0x0F), 64,  8192,  512),
    "m88":    MCU("ATmega88",    (0x1E, 0x93, 0x0A), 64,  8192,  512),
    "m48p":   MCU("ATmega48P",   (0x1E, 0x92, 0x0A), 64,  4096,  256),
    "m48":    MCU("ATmega48",    (0x1E, 0x92, 0x05), 64,  4096,  256),
    "m2560":  MCU("ATmega2560",  (0x1E, 0x98, 0x01), 256, 262144, 4096),
    "m1280":  MCU("ATmega1280",  (0x1E, 0x97, 0x03), 256, 131072, 4096),
    "m32u4":  MCU("ATmega32U4",  (0x1E, 0x95, 0x87), 128, 32768, 1024),
    # ATtiny series
    "t85":    MCU("ATtiny85",    (0x1E, 0x93, 0x0B), 64,  8192,  512),
    "t45":    MCU("ATtiny45",    (0x1E, 0x92, 0x06), 64,  4096,  256),
    "t25":    MCU("ATtiny25",    (0x1E, 0x91, 0x08), 32,  2048,  128),
    "t2313":  MCU("ATtiny2313",  (0x1E, 0x91, 0x0A), 32,  2048,  128),
    "t84":    MCU("ATtiny84",    (0x1E, 0x93, 0x0C), 64,  8192,  512),
    "t44":    MCU("ATtiny44",    (0x1E, 0x92, 0x07), 64,  4096,  256),
    "t24":    MCU("ATtiny24",    (0x1E, 0x91, 0x0B), 32,  2048,  128),
}

# ============================================================================
# Parallel Port Interface
# ============================================================================
class ParallelPort:
    """Low-level parallel port access via Linux ppdev."""
    
    def __init__(self, device: str = "/dev/parport0"):
        self.device = device
        self.fd: Optional[int] = None
        self.data_reg = 0x00
    
    def open(self) -> None:
        """Open and claim the parallel port."""
        self.fd = os.open(self.device, os.O_RDWR)
        fcntl.ioctl(self.fd, PPCLAIM)
        fcntl.ioctl(self.fd, PPDATADIR, struct.pack("i", 0))
        self.data_reg = 0x00
        self._write_data()
    
    def close(self) -> None:
        """Release and close the parallel port."""
        if self.fd is not None:
            try:
                fcntl.ioctl(self.fd, PPRELEASE)
            except OSError:
                pass
            os.close(self.fd)
            self.fd = None
    
    def _write_data(self) -> None:
        """Write the current data register value to hardware."""
        fcntl.ioctl(self.fd, PPWDATA, bytes([self.data_reg & 0xFF]))
    
    def set_data(self, value: int) -> None:
        """Set all DATA pins at once."""
        self.data_reg = value & 0xFF
        self._write_data()
    
    def set_bit(self, bit: int, high: bool) -> None:
        """Set a single DATA pin high or low."""
        if high:
            self.data_reg |= (1 << bit)
        else:
            self.data_reg &= ~(1 << bit)
        self._write_data()
    
    def read_status(self) -> int:
        """Read the STATUS register."""
        buf = bytearray(1)
        fcntl.ioctl(self.fd, PPRSTATUS, buf, True)
        return buf[0]
    
    def __enter__(self) -> "ParallelPort":
        self.open()
        return self
    
    def __exit__(self, *args) -> None:
        self.close()

# ============================================================================
# AVR ISP Programmer
# ============================================================================
class AVRISP:
    """Bit-bang AVR ISP programmer using parallel port."""
    
    # DATA register pin assignments
    BIT_MOSI = 0   # D0, DB-25 pin 2
    BIT_SCK = 1    # D1, DB-25 pin 3
    BIT_RESET = 2  # D2, DB-25 pin 4
    
    def __init__(self, port: ParallelPort, miso_config: int = 10,
                 speed_us: int = 100, invert_reset: bool = False):
        """
        Initialize ISP programmer.
        
        Args:
            port: ParallelPort instance
            miso_config: MISO configuration ID (see MISO_CONFIGS)
            speed_us: Half-period of SPI clock in microseconds
            invert_reset: Invert the RESET signal polarity
        """
        self.port = port
        self.half_period = speed_us / 1_000_000.0
        self.invert_reset = invert_reset
        
        if miso_config not in MISO_CONFIGS:
            raise ValueError(f"Invalid MISO config {miso_config}")
        _, self.miso_mask, self.miso_inverted = MISO_CONFIGS[miso_config]
    
    def _delay(self) -> None:
        """Wait for half a clock period."""
        time.sleep(self.half_period)
    
    def set_reset(self, active: bool) -> None:
        """Control RESET line. active=True holds MCU in reset."""
        drive_high = not active
        if self.invert_reset:
            drive_high = not drive_high
        self.port.set_bit(self.BIT_RESET, drive_high)
    
    def _read_miso(self) -> int:
        """Read MISO bit from STATUS register."""
        status = self.port.read_status()
        bit = 1 if (status & self.miso_mask) else 0
        if self.miso_inverted:
            bit ^= 1
        return bit
    
    def _clock_pulse(self) -> None:
        """Generate one SCK clock pulse."""
        self.port.set_bit(self.BIT_SCK, True)
        self._delay()
        self.port.set_bit(self.BIT_SCK, False)
        self._delay()
    
    def _clock_pulses(self, count: int) -> None:
        """Generate multiple clock pulses."""
        for _ in range(count):
            self._clock_pulse()
    
    def _transfer_byte(self, tx: int) -> int:
        """Transfer one byte via SPI mode 0, return received byte."""
        rx = 0
        for i in range(7, -1, -1):
            # Set MOSI before rising edge
            self.port.set_bit(self.BIT_MOSI, bool((tx >> i) & 1))
            self._delay()
            
            # Rising edge - AVR samples MOSI
            self.port.set_bit(self.BIT_SCK, True)
            self._delay()
            
            # Sample MISO (double-read for PCI card latency)
            self.port.read_status()
            rx = (rx << 1) | self._read_miso()
            
            # Falling edge - AVR shifts out next bit
            self.port.set_bit(self.BIT_SCK, False)
            self._delay()
        
        return rx
    
    def _command(self, a: int, b: int, c: int, d: int) -> Tuple[int, int, int, int]:
        """Send 4-byte ISP command, return all 4 response bytes."""
        return (
            self._transfer_byte(a),
            self._transfer_byte(b),
            self._transfer_byte(c),
            self._transfer_byte(d),
        )
    
    # --- High-level ISP operations ---
    
    def enter_programming_mode(self, retries: int = 3) -> bool:
        """
        Enter serial programming mode.
        
        Returns True on success, False on failure.
        """
        for _ in range(retries):
            self.set_reset(False)
            time.sleep(0.050)
            self.set_reset(True)
            time.sleep(0.050)
            self._clock_pulses(32)
            
            # Programming Enable: response byte 2 should echo 0x53
            r = self._command(0xAC, 0x53, 0x00, 0x00)
            if r[2] == 0x53:
                return True
            
            self.set_reset(False)
            time.sleep(0.050)
        
        return False
    
    def leave_programming_mode(self) -> None:
        """Exit programming mode and let MCU run."""
        self.set_reset(False)
    
    def read_signature(self) -> Tuple[int, int, int]:
        """Read the 3-byte device signature."""
        return (
            self._command(0x30, 0x00, 0x00, 0x00)[3],
            self._command(0x30, 0x00, 0x01, 0x00)[3],
            self._command(0x30, 0x00, 0x02, 0x00)[3],
        )
    
    def chip_erase(self) -> None:
        """Erase the entire chip (flash and EEPROM if not protected)."""
        self._command(0xAC, 0x80, 0x00, 0x00)
        time.sleep(0.010)
    
    def read_fuses(self) -> Dict[str, int]:
        """Read all fuse bytes."""
        return {
            "low": self._command(0x50, 0x00, 0x00, 0x00)[3],
            "high": self._command(0x58, 0x08, 0x00, 0x00)[3],
            "extended": self._command(0x50, 0x08, 0x00, 0x00)[3],
            "lock": self._command(0x58, 0x00, 0x00, 0x00)[3],
        }
    
    def _load_page_word(self, offset: int, low: int, high: int) -> None:
        """Load one word into the page buffer."""
        word_offset = offset // 2
        self._command(0x40, 0x00, word_offset & 0xFF, low)
        self._command(0x48, 0x00, word_offset & 0xFF, high)
    
    def _write_flash_page(self, page_addr: int) -> None:
        """Write the page buffer to flash at the given byte address."""
        word_addr = page_addr // 2
        self._command(0x4C, (word_addr >> 8) & 0xFF, word_addr & 0xFF, 0x00)
        time.sleep(0.005)
    
    def read_flash_byte(self, addr: int) -> int:
        """Read one byte from flash memory."""
        word_addr = addr // 2
        if addr & 1:
            return self._command(0x28, (word_addr >> 8) & 0xFF, word_addr & 0xFF, 0x00)[3]
        else:
            return self._command(0x20, (word_addr >> 8) & 0xFF, word_addr & 0xFF, 0x00)[3]
    
    def program_flash(self, mcu: MCU, data: Dict[int, int], 
                      verify: bool = True, progress: bool = True) -> None:
        """
        Program flash memory.
        
        Args:
            mcu: Target MCU definition
            data: Dict mapping addresses to byte values
            verify: Verify after writing
            progress: Show progress output
        """
        # Build full flash image
        flash = bytearray([0xFF] * mcu.flash_size)
        for addr, byte in data.items():
            if addr >= mcu.flash_size:
                raise ValueError(f"Address 0x{addr:X} exceeds flash size")
            flash[addr] = byte
        
        page_size = mcu.page_size
        pages_written = 0
        
        # Write pages
        for page_start in range(0, mcu.flash_size, page_size):
            page_data = flash[page_start:page_start + page_size]
            
            if all(b == 0xFF for b in page_data):
                continue
            
            for i in range(0, page_size, 2):
                low = page_data[i]
                high = page_data[i + 1] if i + 1 < page_size else 0xFF
                self._load_page_word(i, low, high)
            
            self._write_flash_page(page_start)
            pages_written += 1
            
            if progress:
                print(f"\r  Writing: {pages_written} pages", end="", flush=True)
        
        if progress:
            print(f"\r  Wrote {pages_written} pages ({pages_written * page_size} bytes)")
        
        # Verify
        if verify:
            if progress:
                print("  Verifying...", end="", flush=True)
            
            errors = 0
            for addr, expected in sorted(data.items()):
                if addr >= mcu.flash_size:
                    continue
                actual = self.read_flash_byte(addr)
                if actual != expected:
                    if errors == 0 and progress:
                        print()
                    print(f"    MISMATCH at 0x{addr:04X}: expected 0x{expected:02X}, got 0x{actual:02X}")
                    errors += 1
                    if errors > 10:
                        break
            
            if errors:
                raise RuntimeError(f"Verification failed with {errors} errors")
            
            if progress:
                print(" OK")

# ============================================================================
# Intel HEX Parser
# ============================================================================
def parse_intel_hex(path: str) -> Dict[int, int]:
    """Parse Intel HEX file, return dict of address -> byte."""
    data: Dict[int, int] = {}
    base_addr = 0
    
    pattern = re.compile(
        r'^:([0-9A-Fa-f]{2})([0-9A-Fa-f]{4})([0-9A-Fa-f]{2})'
        r'([0-9A-Fa-f]*)([0-9A-Fa-f]{2})\s*$'
    )
    
    with open(path, 'r') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            
            match = pattern.match(line)
            if not match:
                raise ValueError(f"Invalid HEX format at line {line_num}")
            
            byte_count = int(match.group(1), 16)
            address = int(match.group(2), 16)
            record_type = int(match.group(3), 16)
            data_hex = match.group(4)
            checksum = int(match.group(5), 16)
            
            # Verify checksum
            record = bytes.fromhex(
                match.group(1) + match.group(2) + match.group(3) + data_hex
            )
            if ((~sum(record) + 1) & 0xFF) != checksum:
                raise ValueError(f"Checksum error at line {line_num}")
            
            if record_type == 0x00:  # Data
                for i, byte in enumerate(bytes.fromhex(data_hex)):
                    data[base_addr + address + i] = byte
            elif record_type == 0x01:  # EOF
                break
            elif record_type == 0x02:  # Extended segment address
                base_addr = int(data_hex, 16) << 4
            elif record_type == 0x04:  # Extended linear address
                base_addr = int(data_hex, 16) << 16
    
    return data

# ============================================================================
# Configuration Management
# ============================================================================
@dataclass
class Config:
    """Stored configuration for a parallel port."""
    miso_config: int
    speed_us: int
    invert_reset: bool
    
    def to_dict(self) -> dict:
        return {
            "miso_config": self.miso_config,
            "speed_us": self.speed_us,
            "invert_reset": self.invert_reset,
        }
    
    @classmethod
    def from_dict(cls, d: dict) -> "Config":
        return cls(
            miso_config=d.get("miso_config", 10),
            speed_us=d.get("speed_us", 100),
            invert_reset=d.get("invert_reset", False),
        )

def load_config(port_device: str) -> Optional[Config]:
    """Load saved configuration for a port."""
    if not CONFIG_FILE.exists():
        return None
    try:
        with open(CONFIG_FILE) as f:
            configs = json.load(f)
        if port_device in configs:
            return Config.from_dict(configs[port_device])
    except (json.JSONDecodeError, KeyError):
        pass
    return None

def save_config(port_device: str, config: Config) -> None:
    """Save configuration for a port."""
    CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
    
    configs = {}
    if CONFIG_FILE.exists():
        try:
            with open(CONFIG_FILE) as f:
                configs = json.load(f)
        except json.JSONDecodeError:
            pass
    
    configs[port_device] = config.to_dict()
    
    with open(CONFIG_FILE, 'w') as f:
        json.dump(configs, f, indent=2)

# ============================================================================
# Auto-Detection
# ============================================================================
def auto_detect(port: ParallelPort, verbose: bool = True) -> Optional[Config]:
    """
    Automatically detect the correct configuration for the parallel port.
    
    Tests various MISO configurations and speeds to find one that works.
    Returns Config on success, None if no working configuration found.
    """
    if verbose:
        print("Auto-detecting parallel port configuration...")
        print("Make sure your AVR/Arduino is connected and powered.\n")
    
    # Test speeds from slow to fast
    speeds = [500, 200, 100, 50]
    
    # Test MISO configs: prioritize common ones
    miso_priority = [10, 100, 11, 101, 15, 103, 12, 13, 200, 201]
    
    working_configs: List[Tuple[int, int, bool, Tuple[int, int, int]]] = []
    
    for miso_config in miso_priority:
        for invert_reset in [False, True]:
            for speed in speeds:
                try:
                    isp = AVRISP(port, miso_config=miso_config, 
                                speed_us=speed, invert_reset=invert_reset)
                    
                    if isp.enter_programming_mode(retries=1):
                        sig = isp.read_signature()
                        isp.leave_programming_mode()
                        
                        # Valid AVR signatures start with 0x1E
                        if sig[0] == 0x1E:
                            working_configs.append((miso_config, speed, invert_reset, sig))
                            
                            if verbose:
                                desc = MISO_CONFIGS[miso_config][0]
                                inv = " --invert-reset" if invert_reset else ""
                                print(f"  ✓ Found: --miso {miso_config} --speed {speed}{inv}")
                                print(f"    {desc}")
                                print(f"    Signature: {sig[0]:02X} {sig[1]:02X} {sig[2]:02X}")
                                
                                # Identify MCU
                                for name, mcu in MCUS.items():
                                    if mcu.signature == sig:
                                        print(f"    Detected: {mcu.name}")
                                        break
                                print()
                            
                            # Found one - try to find the fastest speed that works
                            best_speed = speed
                            for faster in [s for s in speeds if s < speed]:
                                isp2 = AVRISP(port, miso_config=miso_config,
                                             speed_us=faster, invert_reset=invert_reset)
                                if isp2.enter_programming_mode(retries=1):
                                    sig2 = isp2.read_signature()
                                    isp2.leave_programming_mode()
                                    if sig2 == sig:
                                        best_speed = faster
                                        if verbose:
                                            print(f"    Also works at --speed {faster}")
                            
                            return Config(miso_config, best_speed, invert_reset)
                    
                    isp.leave_programming_mode()
                    
                except Exception:
                    pass
    
    if verbose:
        print("✗ No working configuration found.\n")
        print("Troubleshooting:")
        print("  • Is the AVR/Arduino powered? (USB connected)")
        print("  • Is GND connected? (DB-25 pins 18-25)")
        print("  • Check wiring:")
        print("      DB-25 pin 2  → MOSI (Arduino pin 11)")
        print("      DB-25 pin 3  → SCK  (Arduino pin 13)")
        print("      DB-25 pin 4  → RESET")
        print("      DB-25 pin 10 ← MISO (Arduino pin 12)")
        print("  • Is ppdev module loaded? (sudo modprobe ppdev)")
    
    return None

# ============================================================================
# Commands
# ============================================================================
def cmd_detect(args) -> int:
    """Auto-detect and save configuration."""
    with ParallelPort(args.port) as port:
        config = auto_detect(port, verbose=True)
        
        if config:
            save_config(args.port, config)
            print(f"Configuration saved to {CONFIG_FILE}")
            print(f"\nYou can now program with:")
            print(f"  sudo python3 paravr.py --hex firmware.hex")
            return 0
        
        return 1

def cmd_program(args) -> int:
    """Program a hex file to the AVR."""
    # Load or use provided configuration
    if args.miso is not None:
        config = Config(args.miso, args.speed, args.invert_reset)
    else:
        config = load_config(args.port)
        if config is None:
            print("No saved configuration. Run --detect first, or specify --miso.")
            return 1
    
    with ParallelPort(args.port) as port:
        isp = AVRISP(port, miso_config=config.miso_config,
                    speed_us=config.speed_us, invert_reset=config.invert_reset)
        
        print("Entering programming mode...")
        if not isp.enter_programming_mode():
            print("Failed to enter programming mode!")
            print("Try running --detect to find the correct configuration.")
            return 1
        print("  OK")
        
        sig = isp.read_signature()
        print(f"Signature: {sig[0]:02X} {sig[1]:02X} {sig[2]:02X}")
        
        # Find MCU
        mcu = MCUS.get(args.mcu)
        if mcu is None:
            print(f"Unknown MCU: {args.mcu}")
            isp.leave_programming_mode()
            return 1
        
        if sig != mcu.signature:
            print(f"  WARNING: Expected {mcu.name} signature "
                  f"{mcu.signature[0]:02X} {mcu.signature[1]:02X} {mcu.signature[2]:02X}")
            if not args.force:
                print("  Use --force to program anyway.")
                isp.leave_programming_mode()
                return 1
        else:
            print(f"  Detected: {mcu.name}")
        
        # Load HEX file
        print(f"Loading {args.hex}...")
        try:
            image = parse_intel_hex(args.hex)
        except Exception as e:
            print(f"  Error: {e}")
            isp.leave_programming_mode()
            return 1
        print(f"  {len(image)} bytes")
        
        # Erase
        if not args.no_erase:
            print("Erasing chip...")
            isp.chip_erase()
            print("  OK")
        
        # Program
        print("Programming...")
        try:
            isp.program_flash(mcu, image, verify=not args.no_verify)
        except RuntimeError as e:
            print(f"  Error: {e}")
            isp.leave_programming_mode()
            return 1
        
        isp.leave_programming_mode()
        print("\n✓ Done!")
        return 0

def cmd_fuses(args) -> int:
    """Read fuse bytes."""
    if args.miso is not None:
        config = Config(args.miso, args.speed, args.invert_reset)
    else:
        config = load_config(args.port)
        if config is None:
            print("No saved configuration. Run --detect first, or specify --miso.")
            return 1
    
    with ParallelPort(args.port) as port:
        isp = AVRISP(port, miso_config=config.miso_config,
                    speed_us=config.speed_us, invert_reset=config.invert_reset)
        
        if not isp.enter_programming_mode():
            print("Failed to enter programming mode!")
            return 1
        
        sig = isp.read_signature()
        print(f"Signature: {sig[0]:02X} {sig[1]:02X} {sig[2]:02X}")
        
        for name, mcu in MCUS.items():
            if mcu.signature == sig:
                print(f"Device:    {mcu.name}")
                break
        
        fuses = isp.read_fuses()
        print(f"\nFuse Low:  0x{fuses['low']:02X}")
        print(f"Fuse High: 0x{fuses['high']:02X}")
        print(f"Fuse Ext:  0x{fuses['extended']:02X}")
        print(f"Lock Bits: 0x{fuses['lock']:02X}")
        
        isp.leave_programming_mode()
        return 0

def cmd_test(args) -> int:
    """Test parallel port I/O."""
    print("=== Parallel Port Test ===\n")
    
    with ParallelPort(args.port) as port:
        print(f"Port: {args.port}")
        status = port.read_status()
        print(f"STATUS register: 0x{status:02X} (binary: {status:08b})\n")
        
        print("Testing DATA pins (outputs)...")
        print("Watch for LED blinks or use a multimeter.\n")
        
        pins = [(0, 2, "MOSI"), (1, 3, "SCK"), (2, 4, "RESET")]
        
        for bit, db25, name in pins:
            print(f"  D{bit} (pin {db25}) {name}: ", end="", flush=True)
            
            port.set_data(1 << bit)
            time.sleep(0.3)
            status_high = port.read_status()
            
            port.set_data(0)
            time.sleep(0.3)
            status_low = port.read_status()
            
            if status_high != status_low:
                delta = status_high ^ status_low
                print(f"STATUS changed (0x{delta:02X}) - loopback detected")
            else:
                print("toggled")
        
        port.set_data(0)
        print("\nTest complete.")
        print("\nIf the SCK LED (pin 13 on Arduino) blinked, outputs are working.")
        print("Run --detect to find the full configuration.")
        
        return 0

def cmd_list_mcus(args) -> int:
    """List supported MCUs."""
    print("Supported MCUs:\n")
    print(f"{'ID':<8} {'Name':<16} {'Signature':<12} {'Flash':<8} {'EEPROM'}")
    print("-" * 60)
    for name, mcu in sorted(MCUS.items()):
        sig = f"{mcu.signature[0]:02X} {mcu.signature[1]:02X} {mcu.signature[2]:02X}"
        flash = f"{mcu.flash_size // 1024}K"
        eeprom = f"{mcu.eeprom_size}"
        print(f"{name:<8} {mcu.name:<16} {sig:<12} {flash:<8} {eeprom}")
    return 0

# ============================================================================
# Main
# ============================================================================
def main() -> int:
    parser = argparse.ArgumentParser(
        description="AVR ISP programmer using parallel port",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --detect                  # Auto-detect and save configuration
  %(prog)s --hex firmware.hex        # Program using saved configuration
  %(prog)s --fuses                   # Read fuse bytes
  %(prog)s --test                    # Test parallel port
  %(prog)s --list-mcus               # List supported MCUs

First-time setup:
  1. Connect wiring (see README.md)
  2. Run: sudo python3 %(prog)s --detect
  3. Program: sudo python3 %(prog)s --hex firmware.hex

Wiring (DB-25 to Arduino Uno):
  Pin 2  (D0)  → Arduino pin 11 (MOSI)
  Pin 3  (D1)  → Arduino pin 13 (SCK)
  Pin 4  (D2)  → Arduino RESET
  Pin 10 (ACK) ← Arduino pin 12 (MISO)
  Pin 18-25    → GND
"""
    )
    
    # Commands
    cmd_group = parser.add_argument_group("Commands")
    cmd_group.add_argument("--detect", action="store_true",
                          help="Auto-detect configuration and save it")
    cmd_group.add_argument("--hex", metavar="FILE",
                          help="Program Intel HEX file to flash")
    cmd_group.add_argument("--fuses", action="store_true",
                          help="Read fuse bytes")
    cmd_group.add_argument("--test", action="store_true",
                          help="Test parallel port I/O")
    cmd_group.add_argument("--list-mcus", action="store_true",
                          help="List supported MCUs")
    
    # Hardware options
    hw_group = parser.add_argument_group("Hardware Options")
    hw_group.add_argument("--port", default="/dev/parport0",
                         help="Parallel port device (default: /dev/parport0)")
    hw_group.add_argument("--miso", type=int, metavar="ID",
                         help=f"MISO configuration ID (see --list-miso)")
    hw_group.add_argument("--speed", type=int, default=100, metavar="US",
                         help="SPI half-period in microseconds (default: 100)")
    hw_group.add_argument("--invert-reset", action="store_true",
                         help="Invert RESET signal polarity")
    hw_group.add_argument("--list-miso", action="store_true",
                         help="List MISO configuration options")
    
    # Programming options
    prog_group = parser.add_argument_group("Programming Options")
    prog_group.add_argument("--mcu", default="m328p",
                           help="Target MCU (default: m328p, see --list-mcus)")
    prog_group.add_argument("--no-erase", action="store_true",
                           help="Skip chip erase before programming")
    prog_group.add_argument("--no-verify", action="store_true",
                           help="Skip verification after programming")
    prog_group.add_argument("--force", action="store_true",
                           help="Program even if signature doesn't match")
    
    args = parser.parse_args()
    
    # Handle --list-miso
    if args.list_miso:
        print("MISO Configuration Options:\n")
        print(f"{'ID':<6} {'Description'}")
        print("-" * 50)
        for config_id, (desc, mask, inv) in sorted(MISO_CONFIGS.items()):
            inv_str = "inv" if inv else "   "
            print(f"{config_id:<6} {desc} [0x{mask:02X} {inv_str}]")
        return 0
    
    # Handle --list-mcus
    if args.list_mcus:
        return cmd_list_mcus(args)
    
    # Handle commands
    if args.detect:
        return cmd_detect(args)
    
    if args.test:
        return cmd_test(args)
    
    if args.fuses:
        return cmd_fuses(args)
    
    if args.hex:
        return cmd_program(args)
    
    # No command specified
    parser.print_help()
    return 1

if __name__ == "__main__":
    sys.exit(main())
