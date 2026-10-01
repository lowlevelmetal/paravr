// Unit tests for the parts of paravr that don't touch the port.

#include <unistd.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>

#include "paravr/hex.hpp"
#include "paravr/mcu.hpp"
#include "paravr/settings.hpp"
#include "test.hpp"

using namespace paravr;
using test::hex_record;

namespace {

const std::string kEof = ":00000001FF";

HexImage parse(const std::string& text) {
    std::istringstream in(text);
    return parse_hex(in, "t.hex");
}

}  // namespace

// --- Intel HEX -------------------------------------------------------------

TEST(hex_reads_data_until_eof) {
    HexImage data = parse(hex_record(0, 0x0000, {1, 2, 3}) + "\n" + hex_record(0, 0x0100, {4}) + "\n" +
                          kEof + "\n" + hex_record(0, 0x0200, {5}) + "\nnot hex at all\n");
    CHECK((data == HexImage{{0, 1}, {1, 2}, {2, 3}, {0x100, 4}}));
}

TEST(hex_accepts_missing_eof) {
    CHECK((parse(hex_record(0, 0x10, {7})) == HexImage{{0x10, 7}}));
}

TEST(hex_tolerates_whitespace_crlf_and_lowercase) {
    std::string lower = hex_record(0, 0xAB, {0xCD});
    for (char& c : lower) c = char(std::tolower(c));
    HexImage data = parse("  " + lower + " \r\n\r\n\t\n" + kEof + "\r\n");
    CHECK((data == HexImage{{0xAB, 0xCD}}));
}

TEST(hex_extended_addresses) {
    HexImage data = parse(hex_record(2, 0, {0x10, 0x00}) + "\n" + hex_record(0, 0x20, {1}) + "\n" +
                          hex_record(4, 0, {0x80, 0x01}) + "\n" + hex_record(0, 0x02, {2}) + "\n" + kEof);
    CHECK((data == HexImage{{0x10020, 1}, {0x80010002, 2}}));
}

TEST(hex_later_records_win) {
    HexImage data = parse(hex_record(0, 0, {1, 2, 3}) + "\n" + hex_record(0, 1, {9}) + "\n" + kEof);
    CHECK((data == HexImage{{0, 1}, {1, 9}, {2, 3}}));
}

TEST(hex_rejects_malformed_records) {
    std::string good = hex_record(0, 0, {1, 2}) + "\n";
    std::string bad_sum = hex_record(0, 0, {1, 2});
    bad_sum.back() = bad_sum.back() == '0' ? '1' : '0';
    CHECK_THROWS(parse(good + bad_sum), "t.hex:2: checksum error");
    CHECK_THROWS(parse(":040000000102F9"), "bad record length");  // says 4 bytes, has 2
    CHECK_THROWS(parse(":0100000G01FE"), "not an Intel HEX record");
    CHECK_THROWS(parse("0100000001FE"), "not an Intel HEX record");
    CHECK_THROWS(parse(":0100000001F"), "not an Intel HEX record");
    CHECK_THROWS(parse(hex_record(4, 0, {1, 2, 3})), "bad address record");
}

TEST(read_hex_reports_missing_file) {
    CHECK_THROWS(read_hex("/nonexistent/x.hex"), "/nonexistent/x.hex: No such file or directory");
}

// --- MCU table -------------------------------------------------------------

TEST(mcu_table_is_consistent) {
    std::set<std::string> ids;
    std::set<Signature> sigs;
    for (const Mcu& m : kMcus) {
        CHECK(ids.insert(m.id).second);
        CHECK(sigs.insert(m.sig).second);
        CHECK(m.sig[0] == 0x1E);
        CHECK(m.page_size % 2 == 0 && m.page_size <= 256);
        CHECK(m.flash_size % m.page_size == 0);
    }
}

TEST(mcu_lookup) {
    CHECK(find_mcu("t85") && std::string(find_mcu("t85")->name) == "ATtiny85");
    CHECK(find_mcu(Signature{0x1E, 0x98, 0x01}) == find_mcu("m2560"));
    CHECK(find_mcu("m999") == nullptr);
    CHECK(find_mcu(Signature{0x1E, 0x00, 0x00}) == nullptr);
}

// --- Settings --------------------------------------------------------------

TEST(miso_pins_follow_the_standard_port) {
    CHECK(find_miso_pin(10) && find_miso_pin(10)->mask == 0x40 && !find_miso_pin(10)->inverted);
    CHECK(find_miso_pin(11) && find_miso_pin(11)->mask == 0x80 && find_miso_pin(11)->inverted);
    CHECK(find_miso_pin(14) == nullptr);
}

TEST(describe_settings) {
    CHECK(describe(Settings{}) == "--miso 10 --speed 100");
    CHECK(describe(Settings{15, true, 50, true}) == "--miso 15 --speed 50 --invert-miso --invert-reset");
}

TEST(settings_round_trip) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("paravr-unit-" + std::to_string(getpid()));
    std::string path = (dir / "sub" / "config").string();

    CHECK(!load_settings(path, "/dev/parport0"));

    save_settings(path, "/dev/parport0", Settings{11, false, 50, true});
    save_settings(path, "/dev/parport1", Settings{15, true, 200, false});
    save_settings(path, "/dev/parport0", Settings{12, true, 500, false});  // replaces
    auto p0 = load_settings(path, "/dev/parport0");
    auto p1 = load_settings(path, "/dev/parport1");
    CHECK(p0 && p0->miso_pin == 12 && p0->invert_miso && p0->speed_us == 500 && !p0->invert_reset);
    CHECK(p1 && p1->miso_pin == 15 && p1->invert_miso && p1->speed_us == 200 && !p1->invert_reset);

    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    CHECK(text ==
          "# device miso-pin invert-miso speed-us invert-reset\n"
          "/dev/parport1 15 1 200 0\n"
          "/dev/parport0 12 1 500 0\n");

    std::ofstream(path) << "garbage\n/dev/parport0 99 0 100 0\n/dev/parport0 13 0\n";
    CHECK(!load_settings(path, "/dev/parport0"));

    fs::remove_all(dir);
}

int main() { return test::run_all(); }
