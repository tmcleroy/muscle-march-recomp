#include "wp/video.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>

namespace wp::video {

namespace {

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void put_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& body) {
    put32(out, static_cast<uint32_t>(body.size()));
    std::vector<uint8_t> tagged(type, type + 4);
    tagged.insert(tagged.end(), body.begin(), body.end());
    out.insert(out.end(), tagged.begin(), tagged.end());
    put32(out, ~crc32(tagged.data(), tagged.size()));
}


std::vector<uint8_t> encode_raw_png(const std::vector<uint8_t>& raw, uint32_t width, uint32_t height, uint8_t color_type) {
    std::vector<uint8_t> deflated = {0x78, 0x01};
    size_t position = 0;
    while (position < raw.size()) {
        size_t length = std::min<size_t>(65535, raw.size() - position);
        deflated.push_back(position + length >= raw.size() ? 1 : 0);
        deflated.push_back(static_cast<uint8_t>(length));
        deflated.push_back(static_cast<uint8_t>(length >> 8));
        deflated.push_back(static_cast<uint8_t>(~length));
        deflated.push_back(static_cast<uint8_t>((~length) >> 8));
        deflated.insert(deflated.end(), raw.begin() + static_cast<std::ptrdiff_t>(position), raw.begin() + static_cast<std::ptrdiff_t>(position + length));
        position += length;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put32(deflated, (b << 16) | a);
    std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> header;
    put32(header, width);
    put32(header, height);
    header.insert(header.end(), {8, color_type, 0, 0, 0});
    put_chunk(file, "IHDR", header);
    put_chunk(file, "IDAT", deflated);
    put_chunk(file, "IEND", {});
    return file;
}

void write_file(const char* path, const std::vector<uint8_t>& file) {
    std::FILE* handle = std::fopen(path, "wb");
    if (handle) {
        std::fwrite(file.data(), 1, file.size(), handle);
        std::fclose(handle);
    }
}

}

std::vector<uint8_t> encode_png(const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(width * 3 + 1) * height);
    for (uint32_t y = 0; y < height; y++) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; x++) {
            uint32_t p = pixels[static_cast<size_t>(y) * width + x];
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p));
        }
    }
    return encode_raw_png(raw, width, height, 2);
}

void save_png(const char* path, const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height) {
    write_file(path, encode_png(pixels, width, height));
}

void save_png_rgba(const char* path, const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; y++) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; x++) {
            uint32_t p = pixels[static_cast<size_t>(y) * width + x];
            raw.push_back(static_cast<uint8_t>(p));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(static_cast<uint8_t>(p >> 24));
        }
    }
    write_file(path, encode_raw_png(raw, width, height, 6));
}

}
