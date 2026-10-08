#pragma once

#include <cstdint>
#include <cstring>

namespace wp {

constexpr uint32_t kAddressMask = 0x1FFFFFFF;
constexpr uint32_t kLockedCacheBase = 0xE0000000;
constexpr uint32_t kLockedCacheSize = 0x4000;
constexpr uint32_t kPhysicalSize = 0x14000000;
constexpr size_t kMemorySize = kPhysicalSize + kLockedCacheSize;

extern uint8_t* g_memory;

constexpr uint32_t kFifoAddress = 0xCC008000;

namespace gx {
void push(uint64_t value, unsigned bytes);
}

namespace dsp {
uint16_t read16(uint32_t address);
void write16(uint32_t address, uint16_t value);
}

namespace ipc {
uint32_t read32(uint32_t address);
void write32(uint32_t address, uint32_t value);
}

constexpr bool dsp_register(uint32_t address) {
    return (address >> 12) == 0xCC005;
}

constexpr bool ipc_register(uint32_t address) {
    return (address >> 8) == 0xCD0000;
}

inline uint8_t* host(uint32_t address) {
    if ((address & 0xF0000000) == kLockedCacheBase) {
        return g_memory + kPhysicalSize + (address & (kLockedCacheSize - 1));
    }
    return g_memory + (address & kAddressMask);
}

inline uint8_t rd8(uint32_t address) {
    return *host(address);
}

inline uint16_t rd16(uint32_t address) {
    if (__builtin_expect(dsp_register(address), 0)) {
        return dsp::read16(address);
    }
    uint16_t value;
    std::memcpy(&value, host(address), sizeof value);
    return __builtin_bswap16(value);
}

inline uint32_t rd32(uint32_t address) {
    if (__builtin_expect(dsp_register(address), 0)) {
        return (static_cast<uint32_t>(dsp::read16(address)) << 16) | dsp::read16(address + 2);
    }
    if (__builtin_expect(ipc_register(address), 0)) {
        return ipc::read32(address);
    }
    uint32_t value;
    std::memcpy(&value, host(address), sizeof value);
    return __builtin_bswap32(value);
}

inline uint64_t rd64(uint32_t address) {
    uint64_t value;
    std::memcpy(&value, host(address), sizeof value);
    return __builtin_bswap64(value);
}

inline uint16_t rd16_reversed(uint32_t address) {
    uint16_t value;
    std::memcpy(&value, host(address), sizeof value);
    return value;
}

inline uint32_t rd32_reversed(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, host(address), sizeof value);
    return value;
}

inline void wr8(uint32_t address, uint8_t value) {
    if (__builtin_expect(address == kFifoAddress, 0)) {
        gx::push(value, 1);
        return;
    }
    *host(address) = value;
}

inline void wr16(uint32_t address, uint16_t value) {
    if (__builtin_expect(address == kFifoAddress, 0)) {
        gx::push(value, 2);
        return;
    }
    if (__builtin_expect(dsp_register(address), 0)) {
        dsp::write16(address, value);
        return;
    }
    value = __builtin_bswap16(value);
    std::memcpy(host(address), &value, sizeof value);
}

inline void wr32(uint32_t address, uint32_t value) {
    if (__builtin_expect(address == kFifoAddress, 0)) {
        gx::push(value, 4);
        return;
    }
    if (__builtin_expect(dsp_register(address), 0)) {
        dsp::write16(address, static_cast<uint16_t>(value >> 16));
        dsp::write16(address + 2, static_cast<uint16_t>(value));
        return;
    }
    if (__builtin_expect(ipc_register(address), 0)) {
        ipc::write32(address, value);
        return;
    }
    value = __builtin_bswap32(value);
    std::memcpy(host(address), &value, sizeof value);
}

inline void wr64(uint32_t address, uint64_t value) {
    if (__builtin_expect(address == kFifoAddress, 0)) {
        gx::push(value, 8);
        return;
    }
    value = __builtin_bswap64(value);
    std::memcpy(host(address), &value, sizeof value);
}

inline void wr16_reversed(uint32_t address, uint16_t value) {
    std::memcpy(host(address), &value, sizeof value);
}

inline void wr32_reversed(uint32_t address, uint32_t value) {
    std::memcpy(host(address), &value, sizeof value);
}

inline float rdf32(uint32_t address) {
    uint32_t bits = rd32(address);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

inline double rdf64(uint32_t address) {
    uint64_t bits = rd64(address);
    double value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

inline void wrf32(uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    wr32(address, bits);
}

inline void wrf64(uint32_t address, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    wr64(address, bits);
}

inline void dcbz(uint32_t address) {
    std::memset(host(address & ~31u), 0, 32);
}

}
