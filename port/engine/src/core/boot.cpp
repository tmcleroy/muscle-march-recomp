#include "wp/boot.h"

#include <cstdio>
#include <vector>

#include "wp/memory.h"

namespace wp {

namespace {

constexpr uint32_t kMem1Size = 0x01800000;
constexpr uint32_t kMem1End = 0x81800000;
constexpr uint32_t kMem2Size = 0x04000000;
constexpr uint32_t kMem2End = 0x93600000;
constexpr uint32_t kMem2ArenaLow = 0x90000800;
constexpr uint32_t kIosHeapSize = 0x00020000;
constexpr uint32_t kBi2Size = 0x2000;
constexpr uint32_t kBusClock = 243000000;
constexpr uint32_t kCpuClock = 729000000;
constexpr uint32_t kPalVideoMode = 1;
constexpr uint32_t kNtscVideoMode = 0;
constexpr uint32_t kVideoControlRegister = 0xCC002002;
constexpr uint16_t kVideoControlPal = 0x0101;
constexpr uint16_t kVideoControlNtsc = 0x0001;
constexpr uint32_t kVideoInterruptRegisters[] = {0xCC002030, 0xCC002034};
constexpr uint16_t kVideoInterruptEnabled = 0x1001;
constexpr uint32_t kHeaderSize = 0x20;

bool read_file(const std::string& path, std::vector<uint8_t>& data) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    uint8_t buffer[65536];
    size_t count;
    while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        data.insert(data.end(), buffer, buffer + count);
    }
    std::fclose(file);
    return true;
}

void write_low_memory(const uint8_t* header, uint32_t fst_address, uint32_t fst_size, uint32_t bi2_address, uint32_t ios) {
    std::memcpy(host(0x80000000), header, kHeaderSize);
    wr32(0x80000020, 0x0D15EA5E);
    wr32(0x80000024, 1);
    wr32(0x80000028, kMem1Size);
    wr32(0x8000002C, 0x00000023);
    wr32(0x80000034, fst_address);
    wr32(0x80000038, fst_address);
    wr32(0x8000003C, fst_size);
    char region = static_cast<char>(header[3]);
    bool ntsc = region == 'E' || region == 'J' || region == 'K' || region == 'W';
    wr32(0x800000CC, ntsc ? kNtscVideoMode : kPalVideoMode);
    wr16(kVideoControlRegister, ntsc ? kVideoControlNtsc : kVideoControlPal);
    for (uint32_t address : kVideoInterruptRegisters) {
        wr16(address, kVideoInterruptEnabled);
    }
    wr32(0x800000EC, kMem1End);
    wr32(0x800000F0, kMem1Size);
    wr32(0x800000F4, bi2_address);
    wr32(0x800000F8, kBusClock);
    wr32(0x800000FC, kCpuClock);

    wr32(0x800030D8, 0xFFFFFFFF);
    wr32(0x800030E4, 0x00008201);
    wr32(0x800030E8, 0x00050000);
    wr32(0x80003100, kMem1Size);
    wr32(0x80003104, kMem1Size);
    wr32(0x80003108, kMem1End);
    wr32(0x80003110, fst_address);
    wr32(0x80003114, 0xDEADBEEF);
    wr32(0x80003118, kMem2Size);
    wr32(0x8000311C, kMem2Size);
    wr32(0x80003120, kMem2End);
    wr32(0x80003124, kMem2ArenaLow);
    wr32(0x80003128, kMem2End - kIosHeapSize);
    wr32(0x8000312C, 0xDEADBEEF);
    wr32(0x80003130, kMem2End - kIosHeapSize);
    wr32(0x80003134, kMem2End);
    wr32(0x80003138, 0x00000011);
    wr32(0x80003140, (ios << 16) | 0x161E);
    wr32(0x80003144, 0x00030310);
    wr32(0x80003148, kMem2End);
    wr32(0x8000314C, kMem2End + 0x20000);
    wr32(0x80003158, 0x0000FF01);
    wr32(0x8000315C, 0x80800113);
    std::memcpy(host(0x80003180), header, 4);
    wr32(0x80003184, 0x80000000);
    wr32(0x80003188, (ios << 16) | 0x151D);
    wr32(0x80003198, 0x03E00000);
    wr32(0x8000319C, 0x80000000);
}

}

bool init_boot_memory(const std::string& extracted_directory) {
    std::vector<uint8_t> header;
    std::vector<uint8_t> bi2;
    std::vector<uint8_t> fst;
    if (!read_file(extracted_directory + "/sys/boot.bin", header) ||
        !read_file(extracted_directory + "/sys/bi2.bin", bi2) ||
        !read_file(extracted_directory + "/sys/fst.bin", fst) || header.size() < kHeaderSize) {
        return false;
    }
    const uint32_t fst_address = (kMem1End - static_cast<uint32_t>(fst.size())) & ~0x1Fu;
    const uint32_t bi2_address = fst_address - kBi2Size;
    std::memcpy(host(bi2_address), bi2.data(), bi2.size());
    std::memcpy(host(fst_address), fst.data(), fst.size());
    write_low_memory(header.data(), fst_address, static_cast<uint32_t>(fst.size()), bi2_address, 56);
    return true;
}

bool init_title_memory(const uint8_t* game_id, uint32_t ios) {
    uint8_t header[kHeaderSize] = {};
    std::memcpy(header, game_id, 6);
    write_low_memory(header, kMem1End, 0, 0, ios);
    return true;
}

}
