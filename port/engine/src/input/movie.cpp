#include "wp/movie.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "wp/features.h"
#include "wp/log.h"

namespace wp::movie {

namespace {

// File: "WPMOVIE" + version byte. Version 2: u16 length + the enabled features (comma-separated),
// then one 35-byte record per sample (little-endian); version 1 has no feature list. Version 3
// (tools/movie_pack.py, for movies kept in git) is version 2 with runs: u16 count + one record.
// Recording always writes version 2 (each sample on its own, safe when the app quits with _Exit).
// Record:
// u8 channel, u32 buttons, u8 pointer_valid, f32 pointer_x, f32 pointer_y,
// u8 motion_valid, f32 accel[3], f32 stick_x, f32 stick_y.
constexpr char kMagic[7] = {'W', 'P', 'M', 'O', 'V', 'I', 'E'};
constexpr uint8_t kVersion = 2;
constexpr size_t kRecordSize = 35;

struct Writer {
    std::FILE* file = nullptr;
    uint64_t count = 0;
};

struct Reader {
    std::vector<uint8_t> data;
    size_t offset = 0;
    std::string features;
    bool ended = false;
    bool mismatch_reported = false;
    size_t start = 0;
};

Writer& writer() {
    static Writer w = [] {
        Writer result;
        if (const char* path = std::getenv("WP_RECORD")) {
            result.file = std::fopen(path, "wb");
            if (result.file) {
                std::fwrite(kMagic, 1, sizeof kMagic, result.file);
                std::fputc(kVersion, result.file);
                std::string list = features::resolved();
                uint8_t length[2] = {static_cast<uint8_t>(list.size()), static_cast<uint8_t>(list.size() >> 8)};
                std::fwrite(length, 1, 2, result.file);
                std::fwrite(list.data(), 1, list.size(), result.file);
                log::write("movie", "recording input to %s", path);
            } else {
                std::fprintf(stderr, "cannot record input to %s\n", path);
            }
        }
        return result;
    }();
    return w;
}

Reader& reader() {
    static Reader r = [] {
        Reader result;
        const char* path = std::getenv("WP_REPLAY");
        if (!path) {
            return result;
        }
        std::FILE* file = std::fopen(path, "rb");
        if (!file) {
            std::fprintf(stderr, "cannot read input movie %s\n", path);
            std::exit(1);
        }
        uint8_t buffer[65536];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
            result.data.insert(result.data.end(), buffer, buffer + got);
        }
        std::fclose(file);
        if (result.data.size() < sizeof kMagic + 1 || std::memcmp(result.data.data(), kMagic, sizeof kMagic) != 0) {
            std::fprintf(stderr, "%s is not an input movie\n", path);
            std::exit(1);
        }
        uint8_t version = result.data[sizeof kMagic];
        result.offset = sizeof kMagic + 1;
        if (version >= 2 && result.data.size() >= result.offset + 2) {
            size_t length = result.data[result.offset] | (result.data[result.offset + 1] << 8);
            result.offset += 2;
            result.features.assign(reinterpret_cast<const char*>(result.data.data() + result.offset), length);
            result.offset += length;
        }
        if (version >= 3) {  // expand the runs
            std::vector<uint8_t> expanded(result.data.begin(), result.data.begin() + static_cast<std::ptrdiff_t>(result.offset));
            for (size_t at = result.offset; at + 2 + kRecordSize <= result.data.size(); at += 2 + kRecordSize) {
                size_t count = result.data[at] | (result.data[at + 1] << 8);
                for (size_t i = 0; i < count; i++) {
                    expanded.insert(expanded.end(), result.data.begin() + static_cast<std::ptrdiff_t>(at + 2),
                                    result.data.begin() + static_cast<std::ptrdiff_t>(at + 2 + kRecordSize));
                }
            }
            result.data.swap(expanded);
        }
        result.start = result.offset;
        std::fprintf(stderr, "replaying %zu input samples from %s (features: %s)\n", (result.data.size() - result.offset) / kRecordSize, path,
                     result.features.empty() ? "none" : result.features.c_str());
        return result;
    }();
    return r;
}

void put32(uint8_t* at, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        at[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}
uint32_t get32(const uint8_t* at) {
    return at[0] | (at[1] << 8) | (at[2] << 16) | (static_cast<uint32_t>(at[3]) << 24);
}
void putf(uint8_t* at, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    put32(at, bits);
}
float getf(const uint8_t* at) {
    uint32_t bits = get32(at);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

}  // namespace

bool recording() {
    return writer().file != nullptr;
}

bool replaying() {
    static const bool value = std::getenv("WP_REPLAY") != nullptr;
    return value;
}

void record(uint32_t channel, const input::Sample& sample) {
    Writer& w = writer();
    if (!w.file) {
        return;
    }
    uint8_t r[kRecordSize];
    r[0] = static_cast<uint8_t>(channel);
    put32(r + 1, sample.buttons);
    r[5] = sample.pointer_valid ? 1 : 0;
    putf(r + 6, sample.pointer_x);
    putf(r + 10, sample.pointer_y);
    r[14] = sample.motion_valid ? 1 : 0;
    putf(r + 15, sample.accel[0]);
    putf(r + 19, sample.accel[1]);
    putf(r + 23, sample.accel[2]);
    putf(r + 27, sample.stick_x);
    putf(r + 31, sample.stick_y);
    std::fwrite(r, 1, sizeof r, w.file);
    if (++w.count % 64 == 0) {
        std::fflush(w.file);  // the app may quit with _Exit
    }
}

bool next(uint32_t channel, input::Sample& sample) {
    Reader& rd = reader();
    sample = input::Sample{};
    if (rd.ended) {
        return false;
    }
    if (rd.offset + kRecordSize > rd.data.size()) {
        rd.ended = true;
        std::fprintf(stderr, "input movie finished\n");
        log::write("movie", "input movie finished");
        return false;
    }
    const uint8_t* r = rd.data.data() + rd.offset;
    if (r[0] != channel && !rd.mismatch_reported) {
        rd.mismatch_reported = true;
        std::fprintf(stderr, "input movie out of sync: recorded channel %u, read channel %u\n", r[0], channel);
    }
    rd.offset += kRecordSize;
    sample.buttons = get32(r + 1);
    sample.pointer_valid = r[5] != 0;
    sample.pointer_x = getf(r + 6);
    sample.pointer_y = getf(r + 10);
    sample.motion_valid = r[14] != 0;
    sample.accel[0] = getf(r + 15);
    sample.accel[1] = getf(r + 19);
    sample.accel[2] = getf(r + 23);
    sample.stick_x = getf(r + 27);
    sample.stick_y = getf(r + 31);
    return true;
}

std::string recorded_features() {
    return replaying() ? reader().features : std::string();
}

bool ended() {
    return replaying() && reader().ended;
}

uint64_t samples_used() {
    return replaying() ? (reader().offset - reader().start) / kRecordSize : writer().count;
}

}  // namespace wp::movie
