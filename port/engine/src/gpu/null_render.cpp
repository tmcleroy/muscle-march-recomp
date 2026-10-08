#include "wp/gx_render.h"

#include "wp/memory.h"

namespace wp::gx::render {

namespace {

uint32_t g_batches = 0;
uint32_t g_vertices = 0;

}

bool guest_range_valid(uint32_t address, size_t size) {
    return static_cast<size_t>(address & kAddressMask) + size <= kPhysicalSize;
}

void take_statistics(uint32_t& batches, uint32_t& vertices, double& seconds) {
    batches = g_batches;
    vertices = g_vertices;
    seconds = 0.0;
    g_batches = 0;
    g_vertices = 0;
}

const char* api_name() {
    return "no renderer";
}

void draw(const ScreenVertex*, uint32_t count) {
    g_batches++;
    g_vertices += count;
}

void copy_to_framebuffer(int, int, int, int, bool, const CopyFilter&) {}

bool present_frame(void*, double) {
    return true;
}

bool read_frame(std::vector<uint32_t>&, uint32_t&, uint32_t&) {
    return false;
}

void dump_copies(const char*) {}

void copy_to_texture(uint32_t, uint32_t, int, int, int, int, bool, uint32_t, bool, bool, bool, const CopyFilter&) {}

void invalidate_textures() {}

void clear(int, int, int, int) {}

}
