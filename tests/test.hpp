#pragma once

// Minimal test harness:  TEST(name) { CHECK(...); }  and main() calls test::run_all().

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> all;
    return all;
}

inline int failures = 0;

struct Register {
    Register(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    std::fprintf(stderr, "%s:%d: %s\n", file, line, what.c_str());
    ++failures;
}

inline int run_all() {
    int failed = 0;
    for (const Case& c : cases()) {
        int before = failures;
        try {
            c.fn();
        } catch (const std::exception& e) {
            fail(c.name, 0, std::string("unexpected exception: ") + e.what());
        }
        bool ok = failures == before;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", c.name);
        failed += !ok;
    }
    std::printf("%zu tests, %d failed\n", cases().size(), failed);
    return failed ? 1 : 0;
}

// One Intel HEX record, checksum included.
inline std::string hex_record(uint8_t type, uint16_t addr, const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> rec = {uint8_t(bytes.size()), uint8_t(addr >> 8), uint8_t(addr), type};
    rec.insert(rec.end(), bytes.begin(), bytes.end());
    uint8_t sum = 0;
    for (uint8_t b : rec) sum += b;
    rec.push_back(uint8_t(-sum));
    std::string text = ":";
    for (uint8_t b : rec) {
        char digits[3];
        std::snprintf(digits, sizeof digits, "%02X", b);
        text += digits;
    }
    return text;
}

}  // namespace test

#define TEST(name)                                                  \
    static void name();                                             \
    static const test::Register name##_registered(#name, name);     \
    static void name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) test::fail(__FILE__, __LINE__, "CHECK(" #cond ") failed"); \
    } while (0)

// expr must throw an exception whose message contains text.
#define CHECK_THROWS(expr, text)                                                          \
    do {                                                                                  \
        std::string what_;                                                                \
        try {                                                                             \
            (void)(expr);                                                                 \
        } catch (const std::exception& e) {                                               \
            what_ = e.what();                                                             \
        }                                                                                 \
        if (what_.find(text) == std::string::npos)                                        \
            test::fail(__FILE__, __LINE__,                                                \
                       std::string(#expr " should throw \"") + (text) + "\", got \"" + what_ + "\""); \
    } while (0)
