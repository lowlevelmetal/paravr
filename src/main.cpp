// paravr - AVR ISP programmer for Linux parallel ports
//
// Bit-bangs the AVR serial programming protocol through ppdev. MOSI, SCK and
// RESET are DATA bits 0-2 (DB-25 pins 2-4); MISO is read from a STATUS pin.
//
// License: MIT

#include <getopt.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

#include "commands.hpp"
#include "paravr/settings.hpp"

namespace {

using paravr::Command;
using paravr::Options;

const char kUsage[] =
    "Usage: paravr COMMAND [OPTIONS]\n"
    "AVR ISP programmer for Linux parallel ports.\n"
    "\n"
    "Commands:\n"
    "  --detect        Find working port settings and save them\n"
    "  --hex FILE      Program an Intel HEX file into flash\n"
    "  --fuses         Read fuse and lock bytes\n"
    "  --test          Toggle the output pins and show the status register\n"
    "  --list-mcus     List supported MCUs\n"
    "\n"
    "Options:\n"
    "  --port DEV      Parallel port device (default /dev/parport0)\n"
    "  --miso PIN      Status pin wired to MISO: 10, 11, 12, 13 or 15 (default 10)\n"
    "  --invert-miso   MISO reads inverted (non-standard port card)\n"
    "  --speed US      SPI half clock period in microseconds (default 100)\n"
    "  --invert-reset  RESET is driven through an inverter\n"
    "  --mcu ID        Program as this MCU even if the signature differs\n"
    "                  (default: identified by signature)\n"
    "  --no-erase      Skip chip erase before programming\n"
    "  --no-verify     Skip verification after programming\n"
    "  -h, --help      Show this help\n"
    "\n"
    "Options given here override the settings saved by --detect.\n"
    "Wiring: DB-25 pin 2 -> MOSI, 3 -> SCK, 4 -> RESET, 10 <- MISO, 18-25 GND.\n";

[[noreturn]] void usage_error(const std::string& msg) {
    std::fprintf(stderr, "paravr: %s\nTry 'paravr --help'.\n", msg.c_str());
    std::exit(2);
}

unsigned parse_unsigned(const char* opt, std::string_view text) {
    unsigned v = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || end != text.data() + text.size())
        usage_error(std::string("invalid value for ") + opt + ": '" + std::string(text) + "'");
    return v;
}

Options parse_args(int argc, char** argv) {
    enum {
        kDetect = 256, kHex, kFuses, kTest, kListMcus, kPort, kMiso, kInvertMiso, kSpeed,
        kInvertReset, kMcu, kNoErase, kNoVerify,
    };
    static const option longopts[] = {
        {"detect", no_argument, nullptr, kDetect},
        {"hex", required_argument, nullptr, kHex},
        {"fuses", no_argument, nullptr, kFuses},
        {"test", no_argument, nullptr, kTest},
        {"list-mcus", no_argument, nullptr, kListMcus},
        {"port", required_argument, nullptr, kPort},
        {"miso", required_argument, nullptr, kMiso},
        {"invert-miso", no_argument, nullptr, kInvertMiso},
        {"speed", required_argument, nullptr, kSpeed},
        {"invert-reset", no_argument, nullptr, kInvertReset},
        {"mcu", required_argument, nullptr, kMcu},
        {"no-erase", no_argument, nullptr, kNoErase},
        {"no-verify", no_argument, nullptr, kNoVerify},
        {"help", no_argument, nullptr, 'h'},
        {},
    };

    Options o;
    auto set_command = [&](Command c) {
        if (o.command != Command::None) usage_error("give only one command");
        o.command = c;
    };
    for (int c; (c = getopt_long(argc, argv, "h", longopts, nullptr)) != -1;) {
        switch (c) {
        case kDetect: set_command(Command::Detect); break;
        case kHex: set_command(Command::Program); o.hex = optarg; break;
        case kFuses: set_command(Command::Fuses); break;
        case kTest: set_command(Command::Test); break;
        case kListMcus: set_command(Command::ListMcus); break;
        case kPort: o.port = optarg; break;
        case kMiso:
            o.miso = int(parse_unsigned("--miso", optarg));
            if (!paravr::find_miso_pin(*o.miso)) usage_error("--miso must be 10, 11, 12, 13 or 15");
            break;
        case kInvertMiso: o.invert_miso = true; break;
        case kSpeed: o.speed = parse_unsigned("--speed", optarg); break;
        case kInvertReset: o.invert_reset = true; break;
        case kMcu:
            o.mcu = paravr::find_mcu(optarg);
            if (!o.mcu) usage_error(std::string("unknown MCU '") + optarg + "' (see --list-mcus)");
            break;
        case kNoErase: o.erase = false; break;
        case kNoVerify: o.verify = false; break;
        case 'h': std::fputs(kUsage, stdout); std::exit(0);
        default: std::fputs("Try 'paravr --help'.\n", stderr); std::exit(2);
        }
    }
    if (optind < argc) usage_error(std::string("unexpected argument '") + argv[optind] + "'");
    if (o.command == Command::None) usage_error("no command given");
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const Options o = parse_args(argc, argv);
    try {
        return paravr::run(o);
    } catch (const std::exception& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "paravr: %s\n", e.what());
        return 1;
    }
}
