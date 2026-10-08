#pragma once

#include <cstdint>
#include <string>

namespace wp::nand {

constexpr int32_t kNotFound = -106;
constexpr int32_t kExists = -105;
constexpr int32_t kInvalid = -101;

bool mount(const std::string& root);
int32_t open(const std::string& path, uint32_t mode);
int32_t read(int32_t handle, uint32_t destination, uint32_t length);
int32_t write(int32_t handle, uint32_t source, uint32_t length);
int32_t seek(int32_t handle, int32_t offset, int32_t whence);
void close(int32_t handle);
int32_t create_file(const std::string& path);
int32_t create_directory(const std::string& path);
int32_t remove(const std::string& path);
int32_t rename(const std::string& from, const std::string& to);
bool exists(const std::string& path);
std::string host_directory(const std::string& path);
bool widescreen();

}
