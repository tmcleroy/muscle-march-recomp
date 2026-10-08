#include "wp/disc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "wp/log.h"
#include "wp/memory.h"

namespace wp::disc {

namespace {

constexpr size_t kEntrySize = 12;
constexpr uint32_t kOffsetShift = 2;
constexpr uint64_t kBootOffset = 0;
constexpr uint64_t kBi2Offset = 0x440;
constexpr uint64_t kApploaderOffset = 0x2440;
constexpr uint32_t kDolOffsetField = 0x420;
constexpr uint32_t kFstOffsetField = 0x424;

struct FileEntry {
    uint64_t offset;
    uint64_t size;
    std::string path;
};

std::vector<FileEntry> g_files;
std::string g_root;

uint32_t be32(const std::vector<uint8_t>& data, size_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
}

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

uint64_t file_size(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return 0;
    }
    std::fseek(file, 0, SEEK_END);
    uint64_t size = static_cast<uint64_t>(std::ftell(file));
    std::fclose(file);
    return size;
}

void add_system_file(const std::string& name, uint64_t offset) {
    std::string path = g_root + "/sys/" + name;
    g_files.push_back({offset, file_size(path), path});
}

bool add_files(const std::vector<uint8_t>& fst) {
    if (fst.size() < kEntrySize) {
        return false;
    }
    uint32_t count = be32(fst, 8);
    size_t names = static_cast<size_t>(count) * kEntrySize;
    if (names > fst.size()) {
        return false;
    }
    std::vector<std::pair<std::string, uint32_t>> directories;
    directories.push_back({"", count});
    for (uint32_t index = 1; index < count; index++) {
        while (directories.size() > 1 && index >= directories.back().second) {
            directories.pop_back();
        }
        size_t entry = index * kEntrySize;
        bool is_directory = fst[entry] != 0;
        uint32_t name_offset = be32(fst, entry) & 0x00FFFFFF;
        std::string name(reinterpret_cast<const char*>(&fst[names + name_offset]));
        std::string parent;
        for (const auto& directory : directories) {
            parent += directory.first;
        }
        if (is_directory) {
            directories.push_back({name + "/", be32(fst, entry + 8)});
        } else {
            uint64_t offset = static_cast<uint64_t>(be32(fst, entry + 4)) << kOffsetShift;
            g_files.push_back({offset, be32(fst, entry + 8), g_root + "/files/" + parent + name});
        }
    }
    return true;
}

}

bool mount(const std::string& extracted_directory) {
    g_root = extracted_directory;
    g_files.clear();
    std::vector<uint8_t> boot;
    std::vector<uint8_t> fst;
    if (!read_file(g_root + "/sys/boot.bin", boot) || !read_file(g_root + "/sys/fst.bin", fst) || boot.size() < 0x440) {
        return false;
    }
    add_system_file("boot.bin", kBootOffset);
    add_system_file("bi2.bin", kBi2Offset);
    add_system_file("apploader.img", kApploaderOffset);
    add_system_file("main.dol", static_cast<uint64_t>(be32(boot, kDolOffsetField)) << kOffsetShift);
    add_system_file("fst.bin", static_cast<uint64_t>(be32(boot, kFstOffsetField)) << kOffsetShift);
    if (!add_files(fst)) {
        return false;
    }
    std::sort(g_files.begin(), g_files.end(), [](const FileEntry& a, const FileEntry& b) { return a.offset < b.offset; });
    return true;
}

void read(uint64_t offset, uint32_t length, uint32_t destination) {
    static const bool logging = std::getenv("WP_LOG_DISC") != nullptr;
    uint8_t* output = host(destination);
    std::fill(output, output + length, 0);
    uint64_t end = offset + length;
    auto it = std::upper_bound(g_files.begin(), g_files.end(), offset,
                               [](uint64_t value, const FileEntry& entry) { return value < entry.offset; });
    if (it != g_files.begin()) {
        --it;
    }
    for (; it != g_files.end() && it->offset < end; ++it) {
        uint64_t file_end = it->offset + it->size;
        if (file_end <= offset) {
            continue;
        }
        uint64_t start = std::max(offset, it->offset);
        uint64_t stop = std::min(end, file_end);
        std::FILE* file = std::fopen(it->path.c_str(), "rb");
        if (logging) {
            std::fprintf(stderr, "disc read %llx+%x -> %08x from %s%s\n", static_cast<unsigned long long>(offset), length,
                         destination, it->path.c_str(), file ? "" : " (missing)");
        }
        log::write("disc", "read %u bytes from %s%s", static_cast<uint32_t>(stop - start), it->path.c_str(), file ? "" : " (missing)");
        if (!file) {
            continue;
        }
        std::fseek(file, static_cast<long>(start - it->offset), SEEK_SET);
        std::fread(output + (start - offset), 1, static_cast<size_t>(stop - start), file);
        std::fclose(file);
    }
}

}
