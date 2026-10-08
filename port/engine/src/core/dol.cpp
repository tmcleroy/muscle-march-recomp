#include "wp/dol.h"

#include <cstdio>
#include <vector>

#include "wp/memory.h"

namespace wp {

namespace {

constexpr int kSectionCount = 18;

uint32_t be32(const std::vector<uint8_t>& data, size_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
}

}

bool load_dol(const std::string& path, uint32_t& entry) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    std::vector<uint8_t> data;
    uint8_t buffer[65536];
    size_t count;
    while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        data.insert(data.end(), buffer, buffer + count);
    }
    std::fclose(file);
    if (data.size() < 0x100) {
        return false;
    }
    for (int i = 0; i < kSectionCount; i++) {
        uint32_t offset = be32(data, i * 4);
        uint32_t address = be32(data, 0x48 + i * 4);
        uint32_t size = be32(data, 0x90 + i * 4);
        if (size == 0) {
            continue;
        }
        if (offset + size > data.size()) {
            return false;
        }
        std::memcpy(host(address), data.data() + offset, size);
    }
    entry = be32(data, 0xE0);
    return true;
}

}
