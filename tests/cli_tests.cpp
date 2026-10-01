// End-to-end tests: run the paravr binary against avrsim, a simulated AVR
// target that LD_PRELOAD puts in place of /dev/parport0.
//
// Usage: cli_tests PARAVR LIBAVRSIM WORKDIR

#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "paravr/hex.hpp"
#include "test.hpp"

extern char** environ;

namespace {

namespace fs = std::filesystem;
using paravr::HexImage;

std::string g_paravr, g_avrsim;
fs::path g_work;

struct Result {
    int status = -1;
    std::string output;  // stdout and stderr
};

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A simulated target with its own flash image and home directory in WORKDIR/name.
class Target {
public:
    explicit Target(std::string name, std::vector<std::string> env = {})
        : dir_(g_work / name), env_(std::move(env)) {
        fs::remove_all(dir_);
        fs::create_directories(dir_ / "home");
    }

    const fs::path& dir() const { return dir_; }

    // Runs paravr with args, wired to this target.
    Result run(const std::vector<std::string>& args) const {
        std::vector<std::string> vars = {
            "LD_PRELOAD=" + g_avrsim,
            "HOME=" + (dir_ / "home").string(),
            "AVRSIM_FLASH=" + flash_file().string(),
            "AVRSIM_VERBOSE=1",
        };
        vars.insert(vars.end(), env_.begin(), env_.end());
        for (char** e = environ; *e; ++e) {
            std::string_view v(*e);
            if (v.rfind("LD_PRELOAD=", 0) && v.rfind("HOME=", 0) && v.rfind("AVRSIM_", 0))
                vars.emplace_back(v);
        }
        std::vector<char*> envp, argv{const_cast<char*>(g_paravr.c_str())};
        for (std::string& v : vars) envp.push_back(v.data());
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        envp.push_back(nullptr);
        argv.push_back(nullptr);

        int fds[2];
        if (pipe(fds) != 0) throw std::runtime_error("pipe failed");
        pid_t pid = fork();
        if (pid < 0) throw std::runtime_error("fork failed");
        if (pid == 0) {
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            close(fds[0]);
            close(fds[1]);
            execve(argv[0], argv.data(), envp.data());
            _exit(127);
        }
        close(fds[1]);
        Result r;
        char buf[4096];
        for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) r.output.append(buf, size_t(n));
        close(fds[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return r;
    }

    // Writes data as an Intel HEX file and returns its path.
    std::string write_hex(const HexImage& data) const {
        fs::path path = dir_ / "image.hex";
        std::ofstream out(path);
        uint32_t upper = 0;
        for (auto it = data.begin(); it != data.end();) {
            uint32_t start = it->first;
            if (start >> 16 != upper) {
                upper = start >> 16;
                out << test::hex_record(4, 0, {uint8_t(upper >> 8), uint8_t(upper)}) << '\n';
            }
            std::vector<uint8_t> bytes;  // up to 16 consecutive bytes within this 64K segment
            while (it != data.end() && it->first == start + bytes.size() && it->first >> 16 == upper &&
                   bytes.size() < 16)
                bytes.push_back((it++)->second);
            out << test::hex_record(0, uint16_t(start), bytes) << '\n';
        }
        out << ":00000001FF\n";
        return path.string();
    }

    std::vector<uint8_t> flash() const {
        std::ifstream in(flash_file(), std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }

    void set_flash(const std::vector<uint8_t>& image) const {
        std::ofstream(flash_file(), std::ios::binary)
            .write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    }

    std::string config() const {
        std::ifstream in(dir_ / "home/.config/paravr/config");
        return {std::istreambuf_iterator<char>(in), {}};
    }

private:
    fs::path flash_file() const { return dir_ / "flash.bin"; }

    fs::path dir_;
    std::vector<std::string> env_;
};

// Deterministic pseudo-random bytes at [start, start + count).
HexImage random_bytes(uint32_t start, uint32_t count) {
    static uint32_t seed = 1;
    HexImage data;
    for (uint32_t i = 0; i < count; ++i) {
        seed = seed * 1103515245 + 12345;
        data[start + i] = uint8_t(seed >> 16);
    }
    return data;
}

// Flash contents after programming data into an erased chip of size bytes.
std::vector<uint8_t> erased_with(const HexImage& data, size_t size) {
    std::vector<uint8_t> flash(size, 0xFF);
    for (auto [addr, byte] : data) flash[addr] = byte;
    return flash;
}

}  // namespace

// The run exited with code and its output contains text.
#define EXPECT_RUN(run, code, text)                                                            \
    do {                                                                                       \
        const Result& r_ = (run);                                                              \
        if (r_.status != (code) || !contains(r_.output, text))                                 \
            test::fail(__FILE__, __LINE__,                                                     \
                       "expected exit " + std::to_string(code) + " and \"" + std::string(text) + \
                           "\", got exit " + std::to_string(r_.status) + ":\n" + r_.output);   \
    } while (0)

// --- Detection -------------------------------------------------------------

TEST(detect_standard_port) {
    Target t("detect_standard_port");
    EXPECT_RUN(t.run({"--detect"}), 0, "Working settings: --miso 10 --speed 50\n");
    CHECK(t.config() ==
          "# device miso-pin invert-miso speed-us invert-reset\n"
          "/dev/parport0 10 0 50 0\n");
}

TEST(detect_finds_pin_and_polarities) {
    const struct {
        std::vector<std::string> env;
        const char* want;
    } cases[] = {
        {{"AVRSIM_PIN=11"}, "--miso 11 --speed 50\n"},
        {{"AVRSIM_PIN=13"}, "--miso 13 --speed 50\n"},
        {{"AVRSIM_RESET_INVERT=1"}, "--miso 10 --speed 50 --invert-reset\n"},
        {{"AVRSIM_CARD_INVERT=1"}, "--miso 10 --speed 50 --invert-miso\n"},
        {{"AVRSIM_PIN=15", "AVRSIM_CARD_INVERT=1", "AVRSIM_RESET_INVERT=1"},
         "--miso 15 --speed 50 --invert-miso --invert-reset\n"},
    };
    for (const auto& c : cases)
        EXPECT_RUN(Target("detect_wiring", c.env).run({"--detect"}), 0,
                   std::string("Working settings: ") + c.want);
}

TEST(detect_slows_down_for_slow_target) {
    Target t("detect_slow", {"AVRSIM_MIN_PHASE_US=120"});
    EXPECT_RUN(t.run({"--detect"}), 0, "Working settings: --miso 10 --speed 200\n");
    EXPECT_RUN(t.run({"--fuses"}), 0, "Lock bits:");  // uses the saved speed
    EXPECT_RUN(t.run({"--fuses", "--speed", "50"}), 1, "target not responding");  // --speed overrides it
}

TEST(detect_without_target) {
    Target t("detect_without_target", {"AVRSIM_PIN=99"});  // MISO reaches no status pin
    EXPECT_RUN(t.run({"--detect"}), 1, "No working settings found");
    CHECK(t.config().empty());
}

// --- Fuses -----------------------------------------------------------------

TEST(read_fuses) {
    Result r = Target("read_fuses").run({"--fuses"});
    EXPECT_RUN(r, 0, "Signature: 1E 95 0F (ATmega328P)\n");
    for (const char* line : {"Low fuse:  0xFF\n", "High fuse: 0xDE\n", "Ext fuse:  0xFD\n", "Lock bits: 0xCF\n"})
        EXPECT_RUN(r, 0, line);
}

TEST(wrong_miso_pin_fails_cleanly) {
    EXPECT_RUN(Target("wrong_miso_pin").run({"--fuses", "--miso", "11"}), 1,
               "paravr: target not responding");
}

// --- Programming -----------------------------------------------------------

TEST(program_atmega328p) {
    Target t("program_atmega328p");
    HexImage data = random_bytes(0, 700);  // a gap, an odd start and a partial last page
    data.merge(random_bytes(901, 599));
    Result r = t.run({"--hex", t.write_hex(data), "--speed", "0"});
    EXPECT_RUN(r, 0, "Done.\n");
    EXPECT_RUN(r, 0, "busy_drops=0");
    CHECK(t.flash() == erased_with(data, 32768));
}

TEST(program_full_flash) {
    Target t("program_full_flash");
    HexImage data = random_bytes(0, 32768);
    EXPECT_RUN(t.run({"--hex", t.write_hex(data), "--speed", "0"}), 0, "Writing: 256/256 pages\n");
    CHECK(t.flash() == erased_with(data, 32768));
}

TEST(program_attiny85) {
    Target t("program_attiny85", {"AVRSIM_MCU=t85"});
    HexImage data = random_bytes(0, 2100);
    EXPECT_RUN(t.run({"--hex", t.write_hex(data), "--speed", "0"}), 0, "(ATtiny85)\n");
    CHECK(t.flash() == erased_with(data, 8192));
}

TEST(program_atmega2560_above_128k) {
    Target t("program_atmega2560", {"AVRSIM_MCU=m2560"});
    HexImage data = random_bytes(0, 1024);
    data.merge(random_bytes(0x20000, 1024));
    data.merge(random_bytes(0x3FF00, 256));
    EXPECT_RUN(t.run({"--hex", t.write_hex(data), "--speed", "0"}), 0, "Done.\n");
    CHECK(t.flash() == erased_with(data, 262144));
}

TEST(program_without_verify) {
    Target t("program_without_verify");
    HexImage data = random_bytes(0, 500);
    Result r = t.run({"--hex", t.write_hex(data), "--speed", "0", "--no-verify"});
    EXPECT_RUN(r, 0, "Done.\n");
    CHECK(!contains(r.output, "Verifying"));
    CHECK(t.flash() == erased_with(data, 32768));
}

TEST(program_without_erase) {
    Target t("program_without_erase");
    std::string hex = t.write_hex(random_bytes(0, 300));
    Result r = t.run({"--hex", hex, "--speed", "0", "--no-erase"});
    EXPECT_RUN(r, 0, "Done.\n");  // a blank chip needs no erase
    CHECK(!contains(r.output, "Erasing"));

    t.set_flash(std::vector<uint8_t>(32768, 0x00));  // flash bits can't be set without an erase
    EXPECT_RUN(t.run({"--hex", hex, "--speed", "0", "--no-erase"}), 1, "verification failed");
}

TEST(oversized_image_leaves_chip_untouched) {
    Target t("oversized_image");
    std::vector<uint8_t> before(32768, 0x00);
    t.set_flash(before);
    EXPECT_RUN(t.run({"--hex", t.write_hex(random_bytes(0x7F80, 256)), "--speed", "0"}), 1,
               "beyond the 32768-byte flash of the ATmega328P");
    CHECK(t.flash() == before);
}

TEST(mcu_override) {
    Target t("mcu_override");  // an ATmega328P programmed as ATmega328: same geometry
    HexImage data = random_bytes(0, 256);
    EXPECT_RUN(t.run({"--hex", t.write_hex(data), "--speed", "0", "--mcu", "m328"}), 0,
               "Warning: programming as ATmega328 as requested by --mcu\n");
    CHECK(t.flash() == erased_with(data, 32768));
}

TEST(bad_hex_is_rejected_before_touching_the_port) {
    Target t("bad_hex");
    std::string rec = test::hex_record(0, 0, {1, 2, 3});
    rec[10] = '2';  // first data byte 01 -> 02 breaks the checksum
    std::ofstream(t.dir() / "bad.hex") << rec << "\n:00000001FF\n";
    Result r = t.run({"--hex", (t.dir() / "bad.hex").string()});
    EXPECT_RUN(r, 1, "bad.hex:1: checksum error");
    CHECK(!contains(r.output, "Entering programming mode"));
}

// --- Other commands --------------------------------------------------------

TEST(test_command_toggles_outputs) {
    Result r = Target("test_command").run({"--test"});
    EXPECT_RUN(r, 0, "D0 (pin 2) MOSI  toggled\n");
    EXPECT_RUN(r, 0, "D2 (pin 4) RESET toggled");
}

TEST(command_line_errors) {
    Target t("command_line_errors");
    EXPECT_RUN(t.run({}), 2, "no command given");
    EXPECT_RUN(t.run({"--detect", "--fuses"}), 2, "give only one command");
    EXPECT_RUN(t.run({"--fuses", "--miso", "14"}), 2, "--miso must be 10, 11, 12, 13 or 15");
    EXPECT_RUN(t.run({"--fuses", "--speed", "-3"}), 2, "invalid value for --speed: '-3'");
    EXPECT_RUN(t.run({"--hex", "x.hex", "--mcu", "m999"}), 2, "unknown MCU 'm999'");
    EXPECT_RUN(t.run({"--fuses", "--port", "/nonexistent"}), 1, "paravr: /nonexistent: No such file or directory");
    EXPECT_RUN(t.run({"--list-mcus"}), 0, "t2313  ATtiny2313  1E 91 0A    2K\n");
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: cli_tests PARAVR LIBAVRSIM WORKDIR\n");
        return 2;
    }
    g_paravr = argv[1];
    g_avrsim = argv[2];
    g_work = argv[3];
    fs::remove_all(g_work);
    fs::create_directories(g_work);
    return test::run_all();
}
