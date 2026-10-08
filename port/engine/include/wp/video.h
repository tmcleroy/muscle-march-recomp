#pragma once

#include <cstdint>
#include <vector>

namespace wp::video {

void start();
// Runs the window on the calling (main) thread when the platform needs that
// (macOS); returns once there is no window loop left to run.
void run_main_thread();
void present();
void update_statistics(double fps);
void save_next_frame(const char* path);
void* window_handle();
// SDL_GPU backend: the device (created by the window thread), and a request to
// draw the latest frame on the window thread.
void* gpu_device();
void request_present();
std::vector<uint8_t> encode_png(const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height);
void save_png(const char* path, const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height);
void save_png_rgba(const char* path, const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height);
bool image_point(long x, long y, long client_width, long client_height, float& out_x, float& out_y);
bool window_focused();
bool client_size(int& width, int& height);
bool key_down(int key);
bool cursor_position(long& x, long& y);
bool cursor_on_image(float& x, float& y);

}
