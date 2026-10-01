#include "paravr/settings.hpp"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

// The config file holds one line per port:
//   <device> <miso-pin> <invert-miso> <speed-us> <invert-reset>

namespace paravr {

const MisoPin* find_miso_pin(int pin) {
    for (const MisoPin& p : kMisoPins)
        if (pin == p.pin) return &p;
    return nullptr;
}

std::string describe(const Settings& s) {
    std::string d = "--miso " + std::to_string(s.miso_pin) + " --speed " + std::to_string(s.speed_us);
    if (s.invert_miso) d += " --invert-miso";
    if (s.invert_reset) d += " --invert-reset";
    return d;
}

std::string config_path() {
    const char* home = std::getenv("HOME");
    if (!home) {
        const passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "";
    }
    return std::string(home) + "/.config/paravr/config";
}

std::optional<Settings> load_settings(const std::string& path, const std::string& device) {
    std::ifstream in(path);
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

void save_settings(const std::string& path, const std::string& device, const Settings& s) {
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

}  // namespace paravr
