#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace wp::settings {

void load(const std::string& path);
bool flag(const char* key, const char* variable);
int number(const char* key, const char* variable);
std::string text(const char* key, const char* variable);
void store(const char* key, const std::string& value);
std::vector<std::string> keys();
uint32_t revision();

class LiveFlag {
public:
    LiveFlag(const char* key, const char* variable) : key_(key), variable_(variable) {}
    bool operator()() const;

private:
    const char* key_;
    const char* variable_;
    mutable std::atomic<uint64_t> cache_{0};
};

class LiveNumber {
public:
    LiveNumber(const char* key, const char* variable) : key_(key), variable_(variable) {}
    int operator()() const;

private:
    const char* key_;
    const char* variable_;
    mutable std::atomic<uint64_t> cache_{0};
};

}
