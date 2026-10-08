#include "wp/hud.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace wp::hud {

namespace {

std::mutex g_mutex;
std::vector<Sprite> g_sprites;
uint64_t g_version = 0;
std::atomic<uint32_t> g_picture_width{0};
std::atomic<uint32_t> g_picture_height{0};

}  // namespace

void show(std::vector<Sprite> sprites) {
    if (sprites.size() > kMaxSprites) {
        sprites.resize(kMaxSprites);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (sprites.empty() && g_sprites.empty()) {
        return;
    }
    g_sprites = std::move(sprites);
    g_version++;
}

bool take(uint64_t& version, std::vector<Sprite>& sprites) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (version == g_version) {
        return false;
    }
    version = g_version;
    sprites = g_sprites;
    return true;
}

void set_picture_size(uint32_t width, uint32_t height) {
    g_picture_width = width;
    g_picture_height = height;
}

void picture_size(uint32_t& width, uint32_t& height) {
    width = g_picture_width;
    height = g_picture_height;
}

void composite(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height) {
    std::vector<Sprite> sprites;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        sprites = g_sprites;
    }
    for (const Sprite& sprite : sprites) {
        const options::Image& image = sprite.image;
        if (image.width == 0 || image.height == 0) {
            continue;
        }
        int x0 = static_cast<int>(sprite.x * static_cast<float>(width));
        int y0 = static_cast<int>(sprite.y * static_cast<float>(height));
        int w = std::max(1, static_cast<int>(sprite.width * static_cast<float>(width)));
        int h = std::max(1, static_cast<int>(sprite.height * static_cast<float>(height)));
        for (int y = std::max(0, y0); y < std::min<int>(static_cast<int>(height), y0 + h); y++) {
            uint32_t sy = static_cast<uint32_t>((y - y0) * static_cast<int>(image.height) / h);
            for (int x = std::max(0, x0); x < std::min<int>(static_cast<int>(width), x0 + w); x++) {
                uint32_t sx = static_cast<uint32_t>((x - x0) * static_cast<int>(image.width) / w);
                uint32_t source = image.pixels[sy * image.width + sx];
                uint32_t alpha = source >> 24;
                uint32_t& target = pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
                uint32_t result = 0;
                for (int shift = 0; shift < 24; shift += 8) {
                    uint32_t s = (source >> shift) & 0xFF;
                    uint32_t t = (target >> shift) & 0xFF;
                    result |= ((s * alpha + t * (255 - alpha)) / 255) << shift;
                }
                target = (target & 0xFF000000u) | result;
            }
        }
    }
}

}  // namespace wp::hud
