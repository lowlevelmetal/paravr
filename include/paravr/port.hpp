#pragma once

#include <cstdint>
#include <string>

namespace paravr {

// A claimed ppdev parallel port with the DATA lines driven as outputs.
class Port {
public:
    explicit Port(const std::string& device);
    ~Port();
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;

    void write(uint8_t data);      // set all DATA lines
    void set(int bit, bool high);  // set one DATA line
    uint8_t status();              // read the STATUS register

private:
    void io(unsigned long request, void* arg);

    std::string device_;
    int fd_;
    uint8_t data_ = 0;
};

}  // namespace paravr
