#include "wp/custom_textures.h"

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#elif defined(WP_SDL_PLATFORM)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace wp::gx::custom_textures {

namespace {

constexpr uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
constexpr uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;
constexpr uint64_t kPrime3 = 0x165667B19E3779F9ull;
constexpr uint64_t kPrime4 = 0x85EBCA77C2B2AE63ull;
constexpr uint64_t kPrime5 = 0x27D4EB2F165667C5ull;
constexpr uint32_t kMaxDimension = 16384;

uint64_t rotl(uint64_t value, int count) {
    return (value << count) | (value >> (64 - count));
}

uint64_t read64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {
        value = (value << 8) | p[i];
    }
    return value;
}

uint32_t read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t round(uint64_t accumulator, uint64_t input) {
    accumulator += input * kPrime2;
    accumulator = rotl(accumulator, 31);
    return accumulator * kPrime1;
}

uint64_t merge(uint64_t accumulator, uint64_t value) {
    accumulator ^= round(0, value);
    return accumulator * kPrime1 + kPrime4;
}

std::string hex64(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return text;
}

std::string utf8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool is_palette(uint32_t format) {
    return format >= 8 && format <= 10;
}

#ifdef _WIN32
template <typename T>
void release(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}
#endif

}

uint64_t xxh64(const void* data, size_t size, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + size;
    uint64_t hash;
    if (size >= 32) {
        uint64_t v1 = seed + kPrime1 + kPrime2;
        uint64_t v2 = seed + kPrime2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - kPrime1;
        const uint8_t* limit = end - 32;
        do {
            v1 = round(v1, read64(p));
            v2 = round(v2, read64(p + 8));
            v3 = round(v3, read64(p + 16));
            v4 = round(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        hash = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        hash = merge(hash, v1);
        hash = merge(hash, v2);
        hash = merge(hash, v3);
        hash = merge(hash, v4);
    } else {
        hash = seed + kPrime5;
    }
    hash += static_cast<uint64_t>(size);
    while (end - p >= 8) {
        hash ^= round(0, read64(p));
        hash = rotl(hash, 27) * kPrime1 + kPrime4;
        p += 8;
    }
    if (end - p >= 4) {
        hash ^= static_cast<uint64_t>(read32(p)) * kPrime1;
        hash = rotl(hash, 23) * kPrime2 + kPrime3;
        p += 4;
    }
    while (p < end) {
        hash ^= *p * kPrime5;
        hash = rotl(hash, 11) * kPrime1;
        p++;
    }
    hash ^= hash >> 33;
    hash *= kPrime2;
    hash ^= hash >> 29;
    hash *= kPrime3;
    hash ^= hash >> 32;
    return hash;
}

PaletteRange used_palette_range(const uint8_t* data, size_t size, uint32_t format) {
    if (!is_palette(format) || size == 0) {
        return {};
    }
    uint32_t low = 0xFFFF;
    uint32_t high = 0;
    auto include = [&](uint32_t index) {
        low = std::min(low, index);
        high = std::max(high, index);
    };
    if (format == 8) {
        for (size_t i = 0; i < size; i++) {
            include(data[i] & 15);
            include(data[i] >> 4);
        }
    } else if (format == 9) {
        for (size_t i = 0; i < size; i++) {
            include(data[i]);
        }
    } else {
        for (size_t i = 0; i + 1 < size; i += 2) {
            include(((static_cast<uint32_t>(data[i]) << 8) | data[i + 1]) & 0x3FFF);
        }
    }
    if (low > high) {
        return {};
    }
    return {low, high + 1 - low};
}

std::string TextureName::full() const {
    return base + "_" + texture + tlut + "_" + format;
}

std::string TextureName::wildcard() const {
    return base + "_" + texture + "_$_" + format;
}

TextureName name_texture(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint32_t format, bool mipmaps, const uint8_t* tlut) {
    TextureName name;
    name.base = "tex1_" + std::to_string(width) + "x" + std::to_string(height) + (mipmaps ? "_m" : "");
    name.texture = hex64(xxh64(data, size));
    if (tlut && is_palette(format)) {
        PaletteRange range = used_palette_range(data, size, format);
        if (range.count > 0) {
            name.tlut = "_" + hex64(xxh64(tlut + range.first * 2, range.count * 2));
        }
    }
    name.format = std::to_string(format);
    return name;
}

std::string level_name(const std::string& name, uint32_t level) {
    return level == 0 ? name : name + "_mip" + std::to_string(level);
}

size_t Index::scan(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return 0;
    }
    size_t added = 0;
    std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error);
    for (std::filesystem::recursive_directory_iterator end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && add(it->path())) {
            added++;
        }
    }
    return added;
}

bool Index::add(const std::filesystem::path& file) {
    if (lowercase(utf8(file.extension())) != ".png") {
        return false;
    }
    std::string key = lowercase(utf8(file.stem()));
    constexpr const char* kArbitrary = "_arb";
    if (key.size() > 4 && key.compare(key.size() - 4, 4, kArbitrary) == 0) {
        key.resize(key.size() - 4);
    }
    if (key.compare(0, 5, "tex1_") != 0) {
        return false;
    }
    return m_files.emplace(key, file).second;
}

const std::filesystem::path* Index::find(const std::string& name) const {
    auto found = m_files.find(lowercase(name));
    return found != m_files.end() ? &found->second : nullptr;
}

std::string Index::resolve(const TextureName& name) const {
    if (m_files.empty()) {
        return "";
    }
    std::string wildcard = name.wildcard();
    if (find(wildcard)) {
        return lowercase(wildcard);
    }
    std::string full = name.full();
    if (find(full)) {
        return lowercase(full);
    }
    return "";
}

size_t Index::size() const {
    return m_files.size();
}

bool decode_png(const std::filesystem::path& path, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
#if defined(WP_SDL_PLATFORM)
    SDL_Surface* loaded = SDL_LoadPNG(path.string().c_str());
    SDL_Surface* converted = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
    bool ok = converted && converted->w > 0 && converted->h > 0 && static_cast<uint32_t>(converted->w) <= kMaxDimension &&
              static_cast<uint32_t>(converted->h) <= kMaxDimension;
    if (ok) {
        width = static_cast<uint32_t>(converted->w);
        height = static_cast<uint32_t>(converted->h);
        pixels.resize(static_cast<size_t>(width) * height);
        for (uint32_t row = 0; row < height; row++) {
            std::memcpy(pixels.data() + static_cast<size_t>(row) * width, static_cast<const uint8_t*>(converted->pixels) + static_cast<size_t>(row) * converted->pitch,
                        static_cast<size_t>(width) * 4);
        }
    }
    SDL_DestroySurface(converted);
    SDL_DestroySurface(loaded);
    return ok;
#elif !defined(_WIN32)
    (void)path;
    (void)pixels;
    (void)width;
    (void)height;
    return false;
#else
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    UINT w = 0;
    UINT h = 0;
    ok = ok && SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0 && w <= kMaxDimension && h <= kMaxDimension;
    if (ok) {
        std::vector<uint32_t> decoded(static_cast<size_t>(w) * h);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(decoded.size() * 4), reinterpret_cast<BYTE*>(decoded.data())));
        if (ok) {
            pixels = std::move(decoded);
            width = w;
            height = h;
        }
    }
    release(converter);
    release(frame);
    release(decoder);
    release(factory);
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    return ok;
#endif
}

}
