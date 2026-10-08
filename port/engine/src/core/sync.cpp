#include "wp/sync.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "wp/memory.h"

// Snapshot format (text, one line per hit):
//   <hit> <address> <lr> <crc32 of each granule (WP_SYNC_GRANULE, hex, default
//   0x100 bytes) of each range>...
// WP_SYNC_RANGES="start-end,start-end" (hex) chooses the memory; the default
// is the game's static data. WP_SYNC_DUMP=<hit> also writes raw bytes of
// every range at that hit to <WP_SYNC_OUT>.<hit>.bin. WP_SYNC_LIMIT stops
// the program after that many hits.

namespace wp {

long g_vi_hits = 0;

namespace {


struct Range {
    uint32_t start;
    uint32_t end;
};

struct Settings {
    std::FILE* out = nullptr;
    std::string path;
    std::vector<Range> ranges;
    long dump = -1;
    long limit = 0;
    uint32_t granule = 0x100;
};

Settings load() {
    Settings s;
    const char* out = std::getenv("WP_SYNC_OUT");
    if (!out) {
        return s;
    }
    s.path = out;
    s.out = std::fopen(out, "w");
    const char* text = std::getenv("WP_SYNC_RANGES");
    std::string ranges = text ? text : "8027dc60-80424cd0";
    size_t pos = 0;
    while (pos < ranges.size()) {
        size_t comma = ranges.find(',', pos);
        std::string item = ranges.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t dash = item.find('-');
        if (dash != std::string::npos) {
            s.ranges.push_back({static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)),
                                static_cast<uint32_t>(std::strtoul(item.c_str() + dash + 1, nullptr, 16))});
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    if (const char* dump = std::getenv("WP_SYNC_DUMP")) {
        s.dump = std::atol(dump);
    }
    if (const char* granule = std::getenv("WP_SYNC_GRANULE")) {
        s.granule = static_cast<uint32_t>(std::strtoul(granule, nullptr, 16));
    }
    if (const char* limit = std::getenv("WP_SYNC_LIMIT")) {
        s.limit = std::atol(limit);
    }
    return s;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

}

void sync_hook(Cpu& c, uint32_t address) {
    // WP_HOOK_PRINT=N: print the first N entries of every hooked function.
    static long print_left = std::getenv("WP_HOOK_PRINT") ? std::atol(std::getenv("WP_HOOK_PRINT")) : 0;
    static const uint32_t print_only = std::getenv("WP_HOOK_ONLY") ? static_cast<uint32_t>(std::strtoul(std::getenv("WP_HOOK_ONLY"), nullptr, 16)) : 0;
    if (print_left > 0 && (!print_only || address == print_only)) {
        print_left--;
        std::fprintf(stderr, "hook %08x lr=%08x r3=%08x r4=%08x r5=%08x r6=%08x f1=%g", address, c.lr, c.r[3], c.r[4], c.r[5], c.r[6], c.f[1]);
        // WP_HOOK_WORDS="off,off,..." (hex, may be negative): also print the words at r3+off.
        if (const char* words = std::getenv("WP_HOOK_WORDS")) {
            for (const char* p = words; *p;) {
                char* end = nullptr;
                long offset = std::strtol(p, &end, 16);
                static const int base = std::getenv("WP_HOOK_BASE") ? std::atoi(std::getenv("WP_HOOK_BASE")) & 31 : 3;
                uint32_t at = c.r[base] + static_cast<uint32_t>(offset);
                bool mapped = (at >> 28) == 0x8 || (at >> 28) == 0x9;
                if (mapped && (at & kAddressMask) + 4 <= kPhysicalSize) {
                    std::fprintf(stderr, " [%lx]=%08x", offset, rd32(at));
                } else {
                    std::fprintf(stderr, " [%lx]=?", offset);
                }
                p = *end == ',' ? end + 1 : end;
                if (end == p && *p != ',') {
                    break;
                }
            }
        }
        // WP_HOOK_STR=N: also print the C string r<N> points to.
        if (const char* reg = std::getenv("WP_HOOK_STR")) {
            uint32_t at = c.r[std::atoi(reg) & 31];
            std::fputs(" str=\"", stderr);
            for (int i = 0; i < 64 && ((at >> 28) == 0x8 || (at >> 28) == 0x9); i++, at++) {
                char ch = static_cast<char>(rd8(at));
                if (!ch) {
                    break;
                }
                std::fputc(ch, stderr);
            }
            std::fputc('"', stderr);
        }
        std::fprintf(stderr, " vi=%ld", g_vi_hits);
        if (const char* abs = std::getenv("WP_HOOK_ABS")) {
            std::fprintf(stderr, " abs=%08x", rd32(static_cast<uint32_t>(std::strtoul(abs, nullptr, 16))));
        }
        std::fputc('\n', stderr);
    }
    if (address == 0x801d5720u) {
        g_vi_hits++;
    }
    static const uint32_t sync_function = std::getenv("WP_SYNC_FN") ? static_cast<uint32_t>(std::strtoul(std::getenv("WP_SYNC_FN"), nullptr, 16)) : 0x801d5720u;
    static Settings settings = load();
    static long hit = 0;
    if (!settings.out || address != sync_function) {
        return;
    }
    std::fprintf(settings.out, "%ld %08x %08x", hit, address, c.lr);
    for (const Range& range : settings.ranges) {
        for (uint32_t page = range.start; page < range.end; page += settings.granule) {
            uint32_t size = std::min(settings.granule, range.end - page);
            std::fprintf(settings.out, " %08x", crc32(host(page), size));
        }
    }
    std::fputc('\n', settings.out);
    if (hit == settings.dump) {
        std::string path = settings.path + "." + std::to_string(hit) + ".bin";
        if (std::FILE* dump = std::fopen(path.c_str(), "wb")) {
            for (const Range& range : settings.ranges) {
                std::fwrite(host(range.start), 1, range.end - range.start, dump);
            }
            std::fclose(dump);
        }
    }
    hit++;
    if (settings.limit && hit >= settings.limit) {
        std::fclose(settings.out);
        std::fflush(stderr);
        std::_Exit(0);
    }
}

}
