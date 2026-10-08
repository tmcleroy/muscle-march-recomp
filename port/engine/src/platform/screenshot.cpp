#include "wp/screenshot.h"

#include <cstdio>

namespace wp::screenshot {

std::string file_name(const char* prefix, const std::tm& time, int attempt) {
    char stamp[96];
    std::snprintf(stamp, sizeof stamp, "_%04d-%02d-%02d_%02d-%02d-%02d", time.tm_year + 1900, time.tm_mon + 1, time.tm_mday, time.tm_hour, time.tm_min,
                  time.tm_sec);
    std::string name = std::string(prefix) + stamp;
    if (attempt > 1) {
        name += "_" + std::to_string(attempt);
    }
    return name + ".png";
}

}
