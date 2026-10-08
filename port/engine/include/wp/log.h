#pragma once

namespace wp::log {

bool enabled();
void write(const char* category, const char* format, ...) __attribute__((format(printf, 2, 3)));
void watch_modules();

}
