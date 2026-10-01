#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>

namespace paravr {

// Address -> byte for every data byte of an Intel HEX file.
using HexImage = std::map<uint32_t, uint8_t>;

// Parses Intel HEX up to the EOF record; name prefixes error messages.
HexImage parse_hex(std::istream& in, const std::string& name);

HexImage read_hex(const std::string& path);

}  // namespace paravr
