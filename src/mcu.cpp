#include "paravr/mcu.hpp"

namespace paravr {

const Mcu* find_mcu(std::string_view id) {
    for (const Mcu& m : kMcus)
        if (id == m.id) return &m;
    return nullptr;
}

const Mcu* find_mcu(const Signature& sig) {
    for (const Mcu& m : kMcus)
        if (sig == m.sig) return &m;
    return nullptr;
}

}  // namespace paravr
