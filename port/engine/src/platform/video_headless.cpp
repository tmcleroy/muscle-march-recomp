#include "wp/video.h"

#include "wp/gx.h"
#include "wp/log.h"

namespace wp::video {

void start() {
    log::write("video", "build %s without a window", WP_BUILD);
}

void run_main_thread() {}

void present() {
    gx::process();
}

void update_statistics(double) {}

void save_next_frame(const char*) {}

void* window_handle() {
    return nullptr;
}

bool image_point(long x, long y, long client_width, long client_height, float& out_x, float& out_y) {
    if (client_width <= 0 || client_height <= 0) {
        return false;
    }
    double u = static_cast<double>(x) / client_width;
    double v = static_cast<double>(y) / client_height;
    if (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0) {
        return false;
    }
    out_x = static_cast<float>(u * 2.0 - 1.0);
    out_y = static_cast<float>(v * 2.0 - 1.0);
    return true;
}

bool window_focused() {
    return false;
}

bool client_size(int&, int&) {
    return false;
}

bool key_down(int) {
    return false;
}

bool cursor_position(long&, long&) {
    return false;
}

bool cursor_on_image(float&, float&) {
    return false;
}

}
