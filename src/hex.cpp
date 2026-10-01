#include "paravr/hex.hpp"

#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace paravr {

namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

}  // namespace

HexImage parse_hex(std::istream& in, const std::string& name) {
    HexImage data;
    uint32_t base = 0;
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        auto error = [&](const char* what) {
            return std::runtime_error(name + ':' + std::to_string(n) + ": " + what);
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

HexImage read_hex(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::system_error(errno, std::generic_category(), path);
    return parse_hex(in, path);
}

}  // namespace paravr
