#include "paravr/isp.hpp"

#include <chrono>
#include <exception>
#include <thread>

#include "paravr/port.hpp"

namespace paravr {

namespace {

constexpr int kMosi = 0;   // D0, DB-25 pin 2
constexpr int kSck = 1;    // D1, DB-25 pin 3
constexpr int kReset = 2;  // D2, DB-25 pin 4

void sleep_us(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

}  // namespace

Isp::Isp(Port& port, const Settings& s)
    : port_(port), half_period_us_(s.speed_us), invert_reset_(s.invert_reset) {
    const MisoPin& pin = *find_miso_pin(s.miso_pin);
    miso_mask_ = pin.mask;
    miso_inverted_ = pin.inverted != s.invert_miso;
}

Isp::~Isp() {
    try {
        set_reset(false);
    } catch (const std::exception&) {
    }
}

bool Isp::enter_programming(int attempts) {
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

uint8_t Isp::query(uint8_t a, uint8_t b, uint8_t c) { return command(a, b, c, 0x00)[3]; }

Signature Isp::signature() {
    return {query(0x30, 0x00, 0x00), query(0x30, 0x00, 0x01), query(0x30, 0x00, 0x02)};
}

void Isp::chip_erase() {
    command(0xAC, 0x80, 0x00, 0x00);
    sleep_us(10000);
}

void Isp::write_page(const Mcu& mcu, uint32_t addr, const uint8_t* data) {
    for (unsigned i = 0; i < mcu.page_size; i += 2) {
        command(0x40, 0x00, uint8_t(i / 2), data[i]);
        command(0x48, 0x00, uint8_t(i / 2), data[i + 1]);
    }
    uint32_t word = addr / 2;
    select_extended(mcu, word);
    command(0x4C, uint8_t(word >> 8), uint8_t(word), 0x00);
    sleep_us(5000);
}

uint8_t Isp::read_flash(const Mcu& mcu, uint32_t addr) {
    uint32_t word = addr / 2;
    select_extended(mcu, word);
    return query(addr & 1 ? 0x28 : 0x20, uint8_t(word >> 8), uint8_t(word));
}

void Isp::delay() { sleep_us(half_period_us_); }

void Isp::set_reset(bool active) { port_.set(kReset, active == invert_reset_); }

bool Isp::read_miso() { return bool(port_.status() & miso_mask_) != miso_inverted_; }

// SPI mode 0, MSB first; MISO is sampled while SCK is high.
uint8_t Isp::transfer(uint8_t tx) {
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

std::array<uint8_t, 4> Isp::command(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return {transfer(a), transfer(b), transfer(c), transfer(d)};
}

// Parts with more than 64K words take address bits 16+ from a separate command.
void Isp::select_extended(const Mcu& mcu, uint32_t word) {
    if (mcu.flash_size <= 0x20000 || int(word >> 16) == ext_addr_) return;
    ext_addr_ = int(word >> 16);
    command(0x4D, 0x00, uint8_t(ext_addr_), 0x00);
}

}  // namespace paravr
