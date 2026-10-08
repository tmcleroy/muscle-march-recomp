#pragma once

#include <cstddef>
#include <cstdint>

namespace Common {

inline uint32_t HashAdler32(const uint8_t* data, size_t length) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < length; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

}
