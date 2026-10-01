#pragma once

#include <optional>
#include <string>

#include "paravr/mcu.hpp"

namespace paravr {

enum class Command { None, Detect, Program, Fuses, Test, ListMcus };

// The parsed command line.
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

// Runs o.command and returns the exit status; throws on I/O and target errors.
int run(const Options& o);

}  // namespace paravr
