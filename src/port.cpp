#include "paravr/port.hpp"

#include <fcntl.h>
#include <linux/ppdev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace paravr {

Port::Port(const std::string& device) : device_(device), fd_(::open(device.c_str(), O_RDWR)) {
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), device_);
    try {
        io(PPCLAIM, nullptr);
        int forward = 0;
        io(PPDATADIR, &forward);
        write(0);
    } catch (...) {
        ::close(fd_);
        throw;
    }
}

Port::~Port() {
    ::ioctl(fd_, PPRELEASE);
    ::close(fd_);
}

void Port::write(uint8_t data) {
    data_ = data;
    io(PPWDATA, &data_);
}

void Port::set(int bit, bool high) { write(high ? data_ | 1 << bit : data_ & ~(1 << bit)); }

uint8_t Port::status() {
    uint8_t s;
    io(PPRSTATUS, &s);
    return s;
}

void Port::io(unsigned long request, void* arg) {
    if (::ioctl(fd_, request, arg) < 0) throw std::system_error(errno, std::generic_category(), device_);
}

}  // namespace paravr
