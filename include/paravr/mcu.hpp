#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace paravr {

using Signature = std::array<uint8_t, 3>;

struct Mcu {
    const char* id;
    const char* name;
    Signature sig;
    unsigned page_size;   // bytes
    unsigned flash_size;  // bytes
};

inline constexpr Mcu kMcus[] = {
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

const Mcu* find_mcu(std::string_view id);
const Mcu* find_mcu(const Signature& sig);

}  // namespace paravr
