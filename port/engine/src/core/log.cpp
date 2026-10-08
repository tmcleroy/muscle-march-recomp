#include "wp/log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace wp::log {

namespace {

using Clock = std::chrono::steady_clock;

struct LogFile {
    std::FILE* file = nullptr;
    Clock::time_point origin = Clock::now();
    std::mutex mutex;

    LogFile() {
        if (const char* path = std::getenv("WP_LOG_FILE")) {
            file = std::fopen(path, "w");
        }
    }
};

LogFile& instance() {
    static LogFile log_file;
    return log_file;
}

}

bool enabled() {
    return instance().file != nullptr;
}

void write(const char* category, const char* format, ...) {
    LogFile& log_file = instance();
    if (!log_file.file) {
        return;
    }
    double seconds = std::chrono::duration<double>(Clock::now() - log_file.origin).count();
    char message[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> lock(log_file.mutex);
    std::fprintf(log_file.file, "[%10.3f] %-7s %s\n", seconds, category, message);
    std::fflush(log_file.file);
}

}
