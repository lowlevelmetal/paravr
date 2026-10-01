#pragma once

#include <array>
#include <cstdint>

#include "paravr/mcu.hpp"
#include "paravr/settings.hpp"

namespace paravr {

class Port;

// AVR serial programming over a Port. Releases RESET when destroyed.
class Isp {
public:
    Isp(Port& port, const Settings& s);
    ~Isp();
    Isp(const Isp&) = delete;
    Isp& operator=(const Isp&) = delete;

    // Pulse RESET and send Programming Enable; the target echoes 0x53 when in sync.
    bool enter_programming(int attempts);

    // Send a read instruction and return the byte the target answers with.
    uint8_t query(uint8_t a, uint8_t b, uint8_t c);

    Signature signature();
    void chip_erase();

    // Fill the page buffer with page_size bytes and commit it at byte address addr.
    void write_page(const Mcu& mcu, uint32_t addr, const uint8_t* data);
    uint8_t read_flash(const Mcu& mcu, uint32_t addr);

private:
    void delay();
    void set_reset(bool active);
    bool read_miso();
    uint8_t transfer(uint8_t tx);
    std::array<uint8_t, 4> command(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
    void select_extended(const Mcu& mcu, uint32_t word);

    Port& port_;
    unsigned half_period_us_;
    bool invert_reset_;
    uint8_t miso_mask_;
    bool miso_inverted_;
    int ext_addr_ = -1;  // extended address last sent, -1 if none
};

}  // namespace paravr
