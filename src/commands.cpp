#include "commands.hpp"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

#include "paravr/hex.hpp"
#include "paravr/isp.hpp"
#include "paravr/port.hpp"
#include "paravr/settings.hpp"

namespace paravr {

namespace {

// Saved settings for the port, overridden by anything given on the command line.
Settings resolve_settings(const Options& o) {
    Settings s = load_settings(config_path(), o.port).value_or(Settings{});
    if (o.miso) s.miso_pin = *o.miso;
    if (o.speed) s.speed_us = *o.speed;
    s.invert_miso |= o.invert_miso;
    s.invert_reset |= o.invert_reset;
    return s;
}

const Mcu* print_signature(const Signature& sig) {
    const Mcu* mcu = find_mcu(sig);
    std::printf("Signature: %02X %02X %02X (%s)\n", sig[0], sig[1], sig[2],
                mcu ? mcu->name : "unknown device");
    return mcu;
}

void enter_programming(Isp& isp) {
    std::printf("Entering programming mode... ");
    std::fflush(stdout);
    if (!isp.enter_programming(3)) {
        std::printf("failed\n");
        throw std::runtime_error("target not responding (check wiring and power, or run --detect)");
    }
    std::printf("OK\n");
}

void progress(const char* label, size_t done, size_t total, const char* unit) {
    // Redraw only when the percentage changes.
    if (done > 0 && done < total && done * 100 / total == (done - 1) * 100 / total) return;
    std::printf("\r%s: %zu/%zu %s", label, done, total, unit);
    if (done == total) std::printf("\n");
    std::fflush(stdout);
}

void program_flash(Isp& isp, const Mcu& mcu, const HexImage& data, bool verify) {
    std::vector<uint8_t> image(mcu.flash_size, 0xFF);
    for (auto [addr, byte] : data) image[addr] = byte;

    std::vector<uint32_t> pages;  // pages holding anything other than erased 0xFF
    for (uint32_t p = 0; p < mcu.flash_size; p += mcu.page_size)
        if (std::any_of(image.begin() + p, image.begin() + p + mcu.page_size,
                        [](uint8_t b) { return b != 0xFF; }))
            pages.push_back(p);

    progress("Writing", 0, pages.size(), "pages");
    for (size_t i = 0; i < pages.size(); ++i) {
        isp.write_page(mcu, pages[i], &image[pages[i]]);
        progress("Writing", i + 1, pages.size(), "pages");
    }
    if (!verify) return;

    struct Mismatch {
        uint32_t addr;
        uint8_t want, got;
    };
    std::vector<Mismatch> bad;
    size_t done = 0;
    progress("Verifying", 0, data.size(), "bytes");
    for (auto [addr, want] : data) {
        uint8_t got = isp.read_flash(mcu, addr);
        if (got != want) {
            bad.push_back({addr, want, got});
            if (bad.size() == 10) break;
        }
        progress("Verifying", ++done, data.size(), "bytes");
    }
    if (bad.empty()) return;
    if (done < data.size()) std::printf("\n");
    for (const Mismatch& m : bad)
        std::printf("  0x%05X: wrote 0x%02X, read 0x%02X\n", m.addr, m.want, m.got);
    throw std::runtime_error("verification failed");
}

int cmd_program(const Options& o) {
    const HexImage data = read_hex(o.hex);
    std::printf("Loaded %s (%zu bytes)\n", o.hex.c_str(), data.size());

    Port port(o.port);
    Isp isp(port, resolve_settings(o));
    enter_programming(isp);
    const Mcu* found = print_signature(isp.signature());
    const Mcu* mcu = o.mcu ? o.mcu : found;
    if (!mcu)
        throw std::runtime_error(
            "unrecognized signature (check wiring and --speed, or name the part with --mcu)");
    if (mcu != found) std::printf("Warning: programming as %s as requested by --mcu\n", mcu->name);
    if (!data.empty() && data.rbegin()->first >= mcu->flash_size) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "HEX data at 0x%X is beyond the %u-byte flash of the %s",
                      data.rbegin()->first, mcu->flash_size, mcu->name);
        throw std::runtime_error(msg);
    }

    if (o.erase) {
        std::printf("Erasing chip... ");
        std::fflush(stdout);
        isp.chip_erase();
        std::printf("OK\n");
    }
    program_flash(isp, *mcu, data, o.verify);
    std::printf("Done.\n");
    return 0;
}

int cmd_fuses(const Options& o) {
    Port port(o.port);
    Isp isp(port, resolve_settings(o));
    enter_programming(isp);
    print_signature(isp.signature());
    std::printf("Low fuse:  0x%02X\n", isp.query(0x50, 0x00, 0x00));
    std::printf("High fuse: 0x%02X\n", isp.query(0x58, 0x08, 0x00));
    std::printf("Ext fuse:  0x%02X\n", isp.query(0x50, 0x08, 0x00));
    std::printf("Lock bits: 0x%02X\n", isp.query(0x58, 0x00, 0x00));
    return 0;
}

// Settings work if the target syncs and answers with an Atmel (0x1E) signature.
std::optional<Signature> probe(Port& port, const Settings& s) {
    Isp isp(port, s);
    if (!isp.enter_programming(1)) return std::nullopt;
    Signature sig = isp.signature();
    if (sig[0] != 0x1E) return std::nullopt;
    return sig;
}

int cmd_detect(const Options& o) {
    std::printf("Probing %s; the target must be connected and powered.\n", o.port.c_str());
    Port port(o.port);
    for (const MisoPin& pin : kMisoPins) {
        for (bool invert_miso : {false, true}) {
            for (bool invert_reset : {false, true}) {
                Settings s{pin.pin, invert_miso, 500, invert_reset};
                auto sig = probe(port, s);
                if (!sig) continue;
                print_signature(*sig);

                // Keep the fastest clock that still reads the same signature.
                for (unsigned speed : {200u, 100u, 50u}) {
                    Settings faster = s;
                    faster.speed_us = speed;
                    if (probe(port, faster) != sig) break;
                    s = faster;
                }
                std::printf("Working settings: %s\n", describe(s).c_str());
                save_settings(config_path(), o.port, s);
                std::printf("Saved to %s\n", config_path().c_str());
                return 0;
            }
        }
    }
    std::fprintf(stderr,
                 "No working settings found. Check that:\n"
                 "  - the target is powered and GND is connected (DB-25 pins 18-25)\n"
                 "  - DB-25 pins 2, 3, 4 go to MOSI, SCK, RESET and MISO goes to pin 10\n"
                 "  - it is a real parallel port, not a USB printer adapter\n");
    return 1;
}

int cmd_test(const Options& o) {
    using namespace std::chrono_literals;
    Port port(o.port);
    uint8_t status = port.status();
    std::printf("Status register: 0x%02X (%s)\n", status, std::bitset<8>(status).to_string().c_str());
    std::printf("Toggling outputs; the SCK LED (Arduino pin 13) should blink.\n");

    static const struct {
        int bit, pin;
        const char* name;
    } lines[] = {{0, 2, "MOSI"}, {1, 3, "SCK"}, {2, 4, "RESET"}};
    for (const auto& l : lines) {
        port.write(uint8_t(1 << l.bit));
        std::this_thread::sleep_for(300ms);
        uint8_t high = port.status();
        port.write(0);
        std::this_thread::sleep_for(300ms);
        uint8_t low = port.status();
        std::printf("  D%d (pin %d) %-5s toggled", l.bit, l.pin, l.name);
        if (high != low) std::printf("; status bits 0x%02X changed with it", high ^ low);
        std::printf("\n");
    }
    return 0;
}

int cmd_list_mcus() {
    std::printf("%-6s %-11s %-9s %s\n", "ID", "Name", "Signature", "Flash");
    for (const Mcu& m : kMcus)
        std::printf("%-6s %-11s %02X %02X %02X %4uK\n", m.id, m.name, m.sig[0], m.sig[1], m.sig[2],
                    m.flash_size / 1024);
    return 0;
}

}  // namespace

int run(const Options& o) {
    switch (o.command) {
    case Command::Detect: return cmd_detect(o);
    case Command::Program: return cmd_program(o);
    case Command::Fuses: return cmd_fuses(o);
    case Command::Test: return cmd_test(o);
    case Command::ListMcus: return cmd_list_mcus();
    case Command::None: break;
    }
    return 2;
}

}  // namespace paravr
