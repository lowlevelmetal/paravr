#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace paravr {

// STATUS register inputs that can carry MISO. Standard ports invert only BUSY.
struct MisoPin {
    int pin;        // DB-25 pin
    uint8_t mask;   // STATUS register bit
    bool inverted;  // port hardware inverts this input
};

inline constexpr MisoPin kMisoPins[] = {
    {10, 0x40, false},  // ACK
    {11, 0x80, true},   // BUSY
    {12, 0x20, false},  // PE
    {13, 0x10, false},  // SELECT
    {15, 0x08, false},  // ERROR
};

const MisoPin* find_miso_pin(int pin);

// How the programmer is wired to the port.
struct Settings {
    int miso_pin = 10;
    bool invert_miso = false;   // MISO reads opposite to the standard port
    unsigned speed_us = 100;    // SPI half clock period
    bool invert_reset = false;  // RESET is driven through an inverter
};

// The settings as command-line options, e.g. "--miso 10 --speed 100".
std::string describe(const Settings& s);

// ~/.config/paravr/config
std::string config_path();

// The settings saved for device in the config file at path, if any.
std::optional<Settings> load_settings(const std::string& path, const std::string& device);

// Saves the settings for device, keeping the entries of other devices.
void save_settings(const std::string& path, const std::string& device, const Settings& s);

}  // namespace paravr
