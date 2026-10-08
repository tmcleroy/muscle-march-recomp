#pragma once

#include <filesystem>
#include <string>
#include <vector>

std::filesystem::path path_from(const std::string& utf8);
std::string utf8_of(const std::filesystem::path& path);

std::string find_program(const std::string& name);
void prepend_path(const std::vector<std::filesystem::path>& folders);
std::string environment_value(const char* name);

class Process {
public:
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process();

    bool start(const std::string& program, const std::vector<std::string>& arguments, const std::filesystem::path& folder, bool low_priority,
               std::string& error);
    size_t read(char* buffer, size_t size);
    int wait();
    void kill();

private:
#ifdef _WIN32
    void* process_ = nullptr;
    void* job_ = nullptr;
    void* output_ = nullptr;
#else
    int pid_ = -1;
    int output_ = -1;
#endif
};

bool start_detached(const std::string& program, const std::filesystem::path& folder);
std::string capture(const std::string& program, const std::vector<std::string>& arguments);
