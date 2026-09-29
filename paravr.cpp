// paravr - AVR ISP programmer for Linux parallel ports
//
// Bit-bangs the AVR serial programming protocol through ppdev. MOSI, SCK and
// RESET are DATA bits 0-2 (DB-25 pins 2-4); MISO is read from a STATUS pin.
//
// License: MIT

#include <fcntl.h>
#include <getopt.h>
#include <linux/ppdev.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using Signature = std::array<uint8_t, 3>;

struct Mcu {
    const char* id;
    const char* name;
    Signature sig;
    unsigned page_size;   // bytes
    unsigned flash_size;  // bytes
};

constexpr Mcu kMcus[] = {
    {"m328p", "ATmega328P", {0x1E, 0x95, 0x0F}, 128, 32768},
    {"m328",  "ATmega328",  {0x1E, 0x95, 0x14}, 128, 32768},
    {"m168p", "ATmega168P", {0x1E, 0x94, 0x0B}, 128, 16384},
    {"m168",  "ATmega168",  {0x1E, 0x94, 0x06}, 128, 16384},
    {"m88p",  "ATmega88P",  {0x1E, 0x93, 0x0F}, 64,  8192},
    {"m88",   "ATmega88",   {0x1E, 0x93, 0x0A}, 64,  8192},
    {"m48p",  "ATmega48P",  {0x1E, 0x92, 0x0A}, 64,  4096},
    {"m48",   "ATmega48",   {0x1E, 0x92, 0x05}, 64,  4096},
    {"m2560", "ATmega2560", {0x1E, 0x98, 0x01}, 256, 262144},
    {"m1280", "ATmega1280", {0x1E, 0x97, 0x03}, 256, 131072},
    {"m32u4", "ATmega32U4", {0x1E, 0x95, 0x87}, 128, 32768},
    {"t85",   "ATtiny85",   {0x1E, 0x93, 0x0B}, 64,  8192},
    {"t45",   "ATtiny45",   {0x1E, 0x92, 0x06}, 64,  4096},
    {"t25",   "ATtiny25",   {0x1E, 0x91, 0x08}, 32,  2048},
    {"t2313", "ATtiny2313", {0x1E, 0x91, 0x0A}, 32,  2048},
    {"t84",   "ATtiny84",   {0x1E, 0x93, 0x0C}, 64,  8192},
    {"t44",   "ATtiny44",   {0x1E, 0x92, 0x07}, 64,  4096},
    {"t24",   "ATtiny24",   {0x1E, 0x91, 0x0B}, 32,  2048},
};

const Mcu* find_mcu(std::string_view id) {
    for (const Mcu& m : kMcus)
        if (id == m.id) return &m;
    return nullptr;
}

const Mcu* find_mcu(const Signature& sig) {
    for (const Mcu& m : kMcus)
        if (sig == m.sig) return &m;
    return nullptr;
}

// STATUS register inputs that can carry MISO. Standard ports invert only BUSY.
struct MisoPin {
    int pin;        // DB-25 pin
    uint8_t mask;   // STATUS register bit
    bool inverted;  // port hardware inverts this input
};

constexpr MisoPin kMisoPins[] = {
    {10, 0x40, false},  // ACK
    {11, 0x80, true},   // BUSY
    {12, 0x20, false},  // PE
    {13, 0x10, false},  // SELECT
    {15, 0x08, false},  // ERROR
};

const MisoPin* find_miso_pin(int pin) {
    for (const MisoPin& p : kMisoPins)
        if (pin == p.pin) return &p;
    return nullptr;
}

struct Settings {
    int miso_pin = 10;
    bool invert_miso = false;   // MISO reads opposite to the standard port
    unsigned speed_us = 100;    // SPI half clock period
    bool invert_reset = false;  // RESET is driven through an inverter
};

void sleep_us(unsigned us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

// A claimed ppdev parallel port with DATA driven as outputs.
class Port {
public:
    explicit Port(const std::string& device)
        : device_(device), fd_(::open(device.c_str(), O_RDWR)) {
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), device_);
        try {
            io(PPCLAIM, nullptr);
            int forward = 0;
            io(PPDATADIR, &forward);
            write(0);
        } catch (...) {
            ::close(fd_);
            throw;
        }
    }
    ~Port() {
        ::ioctl(fd_, PPRELEASE);
        ::close(fd_);
    }
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;

    void write(uint8_t data) {
        data_ = data;
        io(PPWDATA, &data_);
    }
    void set(int bit, bool high) { write(high ? data_ | 1 << bit : data_ & ~(1 << bit)); }
    uint8_t status() {
        uint8_t s;
        io(PPRSTATUS, &s);
        return s;
    }

private:
    void io(unsigned long request, void* arg) {
        if (::ioctl(fd_, request, arg) < 0)
            throw std::system_error(errno, std::generic_category(), device_);
    }

    std::string device_;
    int fd_;
    uint8_t data_ = 0;
};

// AVR serial programming over a Port. Releases RESET when destroyed.
class Isp {
public:
    Isp(Port& port, const Settings& s)
        : port_(port), half_period_us_(s.speed_us), invert_reset_(s.invert_reset) {
        const MisoPin& pin = *find_miso_pin(s.miso_pin);
        miso_mask_ = pin.mask;
        miso_inverted_ = pin.inverted != s.invert_miso;
    }
    ~Isp() {
        try {
            set_reset(false);
        } catch (const std::exception&) {
        }
    }
    Isp(const Isp&) = delete;
    Isp& operator=(const Isp&) = delete;

    // Pulse RESET and send Programming Enable; the target echoes 0x53 when in sync.
    bool enter_programming(int attempts) {
        ext_addr_ = -1;
        for (int i = 0; i < attempts; ++i) {
            set_reset(false);
            sleep_us(50000);
            set_reset(true);
            sleep_us(50000);
            for (int n = 0; n < 32; ++n) {
                port_.set(kSck, true);
                delay();
                port_.set(kSck, false);
                delay();
            }
            if (command(0xAC, 0x53, 0x00, 0x00)[2] == 0x53) return true;
        }
        return false;
    }

    // Send a read instruction and return the byte the target answers with.
    uint8_t query(uint8_t a, uint8_t b, uint8_t c) { return command(a, b, c, 0x00)[3]; }

    Signature signature() {
        return {query(0x30, 0x00, 0x00), query(0x30, 0x00, 0x01), query(0x30, 0x00, 0x02)};
    }

    void chip_erase() {
        command(0xAC, 0x80, 0x00, 0x00);
        sleep_us(10000);
    }

    // Fill the page buffer with page_size bytes and commit it at byte address addr.
    void write_page(const Mcu& mcu, uint32_t addr, const uint8_t* data) {
        for (unsigned i = 0; i < mcu.page_size; i += 2) {
            command(0x40, 0x00, uint8_t(i / 2), data[i]);
            command(0x48, 0x00, uint8_t(i / 2), data[i + 1]);
        }
        uint32_t word = addr / 2;
        select_extended(mcu, word);
        command(0x4C, uint8_t(word >> 8), uint8_t(word), 0x00);
        sleep_us(5000);
    }

    uint8_t read_flash(const Mcu& mcu, uint32_t addr) {
        uint32_t word = addr / 2;
        select_extended(mcu, word);
        return query(addr & 1 ? 0x28 : 0x20, uint8_t(word >> 8), uint8_t(word));
    }

private:
    static constexpr int kMosi = 0;   // D0, DB-25 pin 2
    static constexpr int kSck = 1;    // D1, DB-25 pin 3
    static constexpr int kReset = 2;  // D2, DB-25 pin 4

    void delay() { sleep_us(half_period_us_); }

    void set_reset(bool active) { port_.set(kReset, active == invert_reset_); }

    bool read_miso() { return bool(port_.status() & miso_mask_) != miso_inverted_; }

    // SPI mode 0, MSB first; MISO is sampled while SCK is high.
    uint8_t transfer(uint8_t tx) {
        uint8_t rx = 0;
        for (int i = 7; i >= 0; --i) {
            port_.set(kMosi, tx >> i & 1);
            delay();
            port_.set(kSck, true);
            delay();
            port_.status();  // discarded: gives slow PCI cards time to settle
            rx = rx << 1 | read_miso();
            port_.set(kSck, false);
            delay();
        }
        return rx;
    }

    std::array<uint8_t, 4> command(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        return {transfer(a), transfer(b), transfer(c), transfer(d)};
    }

    // Parts with more than 64K words take address bits 16+ from a separate command.
    void select_extended(const Mcu& mcu, uint32_t word) {
        if (mcu.flash_size <= 0x20000 || int(word >> 16) == ext_addr_) return;
        ext_addr_ = int(word >> 16);
        command(0x4D, 0x00, uint8_t(ext_addr_), 0x00);
    }

    Port& port_;
    unsigned half_period_us_;
    bool invert_reset_;
    uint8_t miso_mask_;
    bool miso_inverted_;
    int ext_addr_ = -1;  // extended address last sent, -1 if none
};

// ---------------------------------------------------------------------------
// Intel HEX

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Returns address -> byte for every data record before the EOF record.
std::map<uint32_t, uint8_t> read_hex(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::system_error(errno, std::generic_category(), path);

    std::map<uint32_t, uint8_t> data;
    uint32_t base = 0;
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        auto error = [&](const char* what) {
            return std::runtime_error(path + ':' + std::to_string(n) + ": " + what);
        };
        size_t first = line.find_first_not_of(" \t\r\n\v\f");
        if (first == std::string::npos) continue;
        std::string_view text(line);
        text = text.substr(first, line.find_last_not_of(" \t\r\n\v\f") + 1 - first);

        if (text[0] != ':' || text.size() % 2 == 0) throw error("not an Intel HEX record");
        std::vector<uint8_t> rec;
        uint8_t sum = 0;
        for (size_t i = 1; i < text.size(); i += 2) {
            int hi = hex_digit(text[i]), lo = hex_digit(text[i + 1]);
            if (hi < 0 || lo < 0) throw error("not an Intel HEX record");
            rec.push_back(uint8_t(hi << 4 | lo));
            sum += rec.back();
        }
        if (rec.size() < 5 || rec.size() != rec[0] + 5u) throw error("bad record length");
        if (sum != 0) throw error("checksum error");

        uint32_t offset = rec[1] << 8 | rec[2];
        const uint8_t* payload = &rec[4];
        switch (rec[3]) {
        case 0x00:  // data
            for (unsigned i = 0; i < rec[0]; ++i) data[base + offset + i] = payload[i];
            break;
        case 0x01:  // end of file
            return data;
        case 0x02:  // extended segment address
        case 0x04:  // extended linear address
            if (rec[0] != 2) throw error("bad address record");
            base = uint32_t(payload[0] << 8 | payload[1]) << (rec[3] == 0x02 ? 4 : 16);
            break;
        }
    }
    return data;
}

// ---------------------------------------------------------------------------
// Saved settings: one line per port in ~/.config/paravr/config

std::string config_path() {
    const char* home = std::getenv("HOME");
    if (!home) {
        const passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "";
    }
    return std::string(home) + "/.config/paravr/config";
}

std::optional<Settings> load_settings(const std::string& device) {
    std::ifstream in(config_path());
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        std::string dev;
        Settings s;
        if (fields >> dev >> s.miso_pin >> s.invert_miso >> s.speed_us >> s.invert_reset &&
            dev == device && find_miso_pin(s.miso_pin))
            return s;
    }
    return std::nullopt;
}

void save_settings(const std::string& device, const Settings& s) {
    const std::string path = config_path();
    std::string out = "# device miso-pin invert-miso speed-us invert-reset\n";
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        std::string dev;
        std::istringstream(line) >> dev;
        if (!dev.empty() && dev[0] != '#' && dev != device) out += line + '\n';
    }
    out += device + ' ' + std::to_string(s.miso_pin) + ' ' + std::to_string(s.invert_miso) + ' ' +
           std::to_string(s.speed_us) + ' ' + std::to_string(s.invert_reset) + '\n';

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path);
    f << out;
    f.close();
    if (!f) throw std::runtime_error("cannot write " + path);
}

// ---------------------------------------------------------------------------
// Commands

enum class Command { None, Detect, Program, Fuses, Test, ListMcus };

struct Options {
    Command command = Command::None;
    std::string hex;
    std::string port = "/dev/parport0";
    std::optional<int> miso;
    std::optional<unsigned> speed;
    bool invert_miso = false;
    bool invert_reset = false;
    const Mcu* mcu = nullptr;
    bool erase = true;
    bool verify = true;
};

// Saved settings for the port, overridden by anything given on the command line.
Settings resolve_settings(const Options& o) {
    Settings s = load_settings(o.port).value_or(Settings{});
    if (o.miso) s.miso_pin = *o.miso;
    if (o.speed) s.speed_us = *o.speed;
    s.invert_miso |= o.invert_miso;
    s.invert_reset |= o.invert_reset;
    return s;
}

std::string describe(const Settings& s) {
    std::string d = "--miso " + std::to_string(s.miso_pin) + " --speed " + std::to_string(s.speed_us);
    if (s.invert_miso) d += " --invert-miso";
    if (s.invert_reset) d += " --invert-reset";
    return d;
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

void program_flash(Isp& isp, const Mcu& mcu, const std::map<uint32_t, uint8_t>& data, bool verify) {
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
    const auto data = read_hex(o.hex);
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
                save_settings(o.port, s);
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
        sleep_us(300000);
        uint8_t high = port.status();
        port.write(0);
        sleep_us(300000);
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

// ---------------------------------------------------------------------------
// Command line

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
            if (!find_miso_pin(*o.miso)) usage_error("--miso must be 10, 11, 12, 13 or 15");
            break;
        case kInvertMiso: o.invert_miso = true; break;
        case kSpeed: o.speed = parse_unsigned("--speed", optarg); break;
        case kInvertReset: o.invert_reset = true; break;
        case kMcu:
            o.mcu = find_mcu(optarg);
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

}  // namespace

int main(int argc, char** argv) {
    const Options o = parse_args(argc, argv);
    try {
        return run(o);
    } catch (const std::exception& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "paravr: %s\n", e.what());
        return 1;
    }
}
