#pragma once
// HUD sprites: small images drawn over the game picture, for optional features' on-screen guides.
// The game is never changed: the renderers blend these over the finished frame. A sprite's
// position and size are fractions of the game picture (0..1, from its top left), so it follows the
// picture at any window size. Pixels are 0xAARRGGBB, not premultiplied.

#include <cstdint>
#include <vector>

#include "wp/options.h"

namespace wp::hud {

struct Sprite {
    options::Image image;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

constexpr size_t kMaxSprites = 8;

// Replaces what is shown (at most kMaxSprites); an empty list shows nothing.
void show(std::vector<Sprite> sprites);
// For the renderers: the current sprites when they changed since `version`.
bool take(uint64_t& version, std::vector<Sprite>& sprites);
// The size in pixels the game picture was last presented at (0 before the first frame), so
// sprites can be drawn sharp.
void set_picture_size(uint32_t width, uint32_t height);
void picture_size(uint32_t& width, uint32_t& height);
// The size of what the picture was presented in (the window's drawing surface), when the renderer
// reports it (0 otherwise): with picture_size, where the picture is in the window.
void set_window_size(uint32_t width, uint32_t height);
void window_size(uint32_t& width, uint32_t& height);
// Blends the current sprites into a captured game picture (tests, WP_CAPTURE_HUD=1).
void composite(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height);

}  // namespace wp::hud
