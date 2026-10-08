#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wp::gx {

struct ScreenVertex {
    float x;
    float y;
    float z;
    float w;
    float color[2][4];
    float uv[8][3];
};

const uint32_t* bp_registers();
const uint32_t* konst_registers();
const uint32_t* xf_registers();

}

namespace wp::gx::render {

struct CopyFilter {
    uint32_t coefficients[3] = {0, 64, 0};
    float gamma = 1.0f;
    bool clamp_top = true;
    bool clamp_bottom = true;
};

bool guest_range_valid(uint32_t address, size_t size);
void take_statistics(uint32_t& batches, uint32_t& vertices, double& seconds);

const char* api_name();
void draw(const ScreenVertex* vertices, uint32_t count);
void copy_to_framebuffer(int x, int y, int width, int height, bool depth, const CopyFilter& filter);
bool present_frame(void* window, double aspect);
// SDL_GPU backend only: draw the last finished frame to the window (window thread).
bool present_to_window(void* window, double aspect);
bool read_frame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height);
void dump_copies(const char* prefix);
void copy_to_texture(uint32_t address, uint32_t stride, int x, int y, int width, int height, bool half, uint32_t format, bool intensity, bool depth, bool alpha,
                     const CopyFilter& filter);
void invalidate_textures();
void load_tlut(uint32_t address, uint32_t tmem_offset, uint32_t bytes);
void clear(int x, int y, int width, int height);

}
