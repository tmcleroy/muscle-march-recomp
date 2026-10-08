#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace wp::gx::custom_textures {

uint64_t xxh64(const void* data, size_t size, uint64_t seed = 0);

struct PaletteRange {
    uint32_t first = 0;
    uint32_t count = 0;
};

PaletteRange used_palette_range(const uint8_t* data, size_t size, uint32_t format);

struct TextureName {
    std::string base;
    std::string texture;
    std::string tlut;
    std::string format;

    std::string full() const;
    std::string wildcard() const;
};

TextureName name_texture(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint32_t format, bool mipmaps, const uint8_t* tlut);
std::string level_name(const std::string& name, uint32_t level);

class Index {
public:
    size_t scan(const std::filesystem::path& directory);
    bool add(const std::filesystem::path& file);
    const std::filesystem::path* find(const std::string& name) const;
    std::string resolve(const TextureName& name) const;
    size_t size() const;

private:
    std::unordered_map<std::string, std::filesystem::path> m_files;
};

bool decode_png(const std::filesystem::path& path, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height);

}
