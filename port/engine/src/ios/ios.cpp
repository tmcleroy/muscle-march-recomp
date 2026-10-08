#include "wp/ios.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#include "wp/bluetooth.h"
#include "wp/cpu.h"
#include "wp/disc.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/nand.h"
#include "wp/title.h"

namespace wp::ios {

namespace {

constexpr uint32_t kTitleIdHigh = 0x00010000;
constexpr uint32_t kGameIdAddress = 0x80000000;
constexpr uint32_t kEsGetTitleId = 0x20;
constexpr uint32_t kEsGetConsumption = 0x16;
constexpr uint32_t kEsGetDiscTicketView = 0x1B;
constexpr uint32_t kVectorSize = 8;

constexpr uint32_t kOpen = 1;
constexpr uint32_t kClose = 2;
constexpr uint32_t kRead = 3;
constexpr uint32_t kWrite = 4;
constexpr uint32_t kSeek = 5;
constexpr uint32_t kIoctl = 6;
constexpr uint32_t kIoctlv = 7;

constexpr uint32_t kDiInquiry = 0x12;
constexpr uint32_t kDiRead = 0x71;
constexpr uint32_t kDiUnencryptedRead = 0x8D;
constexpr uint32_t kDiReportKey = 0xA4;
constexpr uint32_t kDiRequestError = 0xE0;
constexpr uint32_t kDiSuccess = 1;
constexpr uint32_t kDiError = 2;
constexpr uint32_t kDiErrorOutOfRange = 0x00052100;
constexpr uint32_t kDiErrorInvalidField = 0x00053100;
constexpr uint64_t kDiscSize = 0x118240000ull;
constexpr uint32_t kDriveInfoSize = 0x20;
constexpr const char* kFileSystem = "/dev/fs";

void log_once(const char* kind, const std::string& device, uint32_t command) {
    static std::set<std::string> seen;
    char text[160];
    std::snprintf(text, sizeof text, "%s %s command %#x", kind, device.c_str(), command);
    if (seen.insert(text).second) {
        std::fprintf(stderr, "IOS %s", text);
        std::fputc(10, stderr);
    }
}


constexpr uint32_t kFsCreateDirectory = 0x03;
constexpr uint32_t kFsGetAttributes = 0x06;
constexpr uint32_t kFsDelete = 0x07;
constexpr uint32_t kFsRename = 0x08;
constexpr uint32_t kFsCreateFile = 0x09;
constexpr uint32_t kFsGetFileStats = 0x0B;
constexpr uint32_t kFsReadDirectory = 0x04;
constexpr uint32_t kFsGetUsage = 0x0C;
constexpr uint64_t kFsClusterSize = 0x4000;
constexpr uint64_t kIpcOverheadTicks = 2700;
constexpr uint64_t kDefaultReplyTicks = 4000;
constexpr uint64_t kSuperblockWriteTicks = 3170000;
constexpr uint64_t kClusterWriteTicks = 300000;
constexpr uint64_t kClusterReadTicks = 115000;
constexpr uint64_t kFreeClusterCheckTicks = 1000;
constexpr uint64_t kInvalidPathTicks = 300;
constexpr uint64_t kLookupTicks = 680;
constexpr uint64_t kSplitLookupTicks = 1000;
constexpr uint64_t kSplitComponentTicks = 340;
constexpr uint32_t kFsPathOffset = 6;
constexpr uint32_t kFsPathSize = 64;
constexpr uint32_t kFsNameSize = 13;

struct Device {
    std::string path;
    int32_t file = -1;
    bool superblock_flush = false;
};

std::map<int32_t, Device> g_devices;
int32_t g_next_descriptor = 1;
uint32_t g_di_last_error = 0;
int32_t g_cache_descriptor = -1;
uint32_t g_cache_chain = 0;
bool g_dirty_cache = false;

std::string read_string(uint32_t address, uint32_t limit = 256) {
    std::string text;
    for (char ch; (ch = static_cast<char>(rd8(address))) != 0 && text.size() < limit; address++) {
        text.push_back(ch);
    }
    return text;
}

uint64_t lookup_ticks(const std::string& path, bool split) {
    uint64_t components = static_cast<uint64_t>(std::count(path.begin(), path.end(), '/'));
    if (components == 0) {
        return 0;
    }
    if (path.back() == '/') {
        return kInvalidPathTicks;
    }
    return split ? kSplitLookupTicks + kSplitComponentTicks * components : kLookupTicks * components;
}

uint64_t memcpy_ticks(uint32_t size) {
    return static_cast<uint64_t>(0.636 * size + 150.0);
}

bool has_cache(int32_t descriptor, uint32_t offset) {
    return g_cache_descriptor == descriptor && g_cache_chain == offset / kFsClusterSize;
}

uint64_t flush_cache() {
    if (g_cache_descriptor < 0 || !g_dirty_cache) {
        return 0;
    }
    g_dirty_cache = false;
    auto device = g_devices.find(g_cache_descriptor);
    if (device != g_devices.end()) {
        device->second.superblock_flush = true;
    }
    return kClusterWriteTicks;
}

uint64_t populate_cache(int32_t descriptor, uint32_t offset, uint32_t size) {
    if (has_cache(descriptor, offset)) {
        return 0;
    }
    uint64_t ticks = flush_cache();
    if ((offset % kFsClusterSize != 0 || offset != size) && offset < size) {
        ticks += kClusterReadTicks;
    }
    g_cache_descriptor = descriptor;
    g_cache_chain = static_cast<uint32_t>(offset / kFsClusterSize);
    return ticks;
}

uint64_t read_write_ticks(int32_t descriptor, Device& device, bool write, uint32_t size) {
    int32_t position = nand::seek(device.file, 0, 1);
    int32_t end = nand::seek(device.file, 0, 2);
    nand::seek(device.file, position, 0);
    uint32_t offset = static_cast<uint32_t>(position);
    uint32_t file_size = static_cast<uint32_t>(end);
    uint32_t count = size;
    if (!write && count + offset > file_size) {
        count = file_size > offset ? file_size - offset : 0;
    }
    uint64_t ticks = 0;
    while (count != 0) {
        uint32_t length;
        if (!has_cache(descriptor, offset) && count >= kFsClusterSize && offset % kFsClusterSize == 0) {
            ticks += write ? kClusterWriteTicks : kClusterReadTicks;
            length = static_cast<uint32_t>(kFsClusterSize);
            if (write) {
                device.superblock_flush = true;
            }
        } else {
            ticks += populate_cache(descriptor, offset, file_size);
            uint32_t start = offset - g_cache_chain * static_cast<uint32_t>(kFsClusterSize);
            length = std::min(static_cast<uint32_t>(kFsClusterSize) - start, count);
            ticks += memcpy_ticks(length);
            if (write) {
                ticks += kFreeClusterCheckTicks;
            }
            g_dirty_cache = write;
            if (write && (offset + length) % kFsClusterSize == 0) {
                ticks += flush_cache();
            }
        }
        offset += length;
        count -= length;
    }
    return ticks;
}

std::string device_name(int32_t descriptor) {
    auto it = g_devices.find(descriptor);
    return it == g_devices.end() ? "?" : it->second.path;
}

int32_t di_command(uint32_t command, uint32_t input, uint32_t output) {
    switch (command) {
    case kDiInquiry:
        std::memset(host(output), 0, kDriveInfoSize);
        return kDiSuccess;
    case kDiRead:
        disc::read(static_cast<uint64_t>(rd32(input + 8)) << 2, rd32(input + 4), output);
        return kDiSuccess;
    case kDiUnencryptedRead: {
        uint64_t end = (static_cast<uint64_t>(rd32(input + 8)) + rd32(input + 4) / 4) * 4;
        if (end > kDiscSize) {
            g_di_last_error = kDiErrorOutOfRange;
            return kDiError;
        }
        return kDiSuccess;
    }
    case kDiReportKey:
        g_di_last_error = kDiErrorInvalidField;
        return kDiError;
    case kDiRequestError:
        wr32(output, g_di_last_error);
        g_di_last_error = 0;
        return kDiSuccess;
    default:
        log_once("ioctl", "/dev/di", command);
        return kDiSuccess;
    }
}

int32_t fs_command(uint32_t command, uint32_t input, uint32_t output, uint64_t& ticks) {
    static const bool trace = std::getenv("WP_LOG_FS") != nullptr;
    if (trace) {
        std::fprintf(stderr, "FS ioctl %#x %s\n", command, read_string(command == kFsCreateDirectory || command == kFsCreateFile ? input + kFsPathOffset : input, kFsPathSize).c_str());
        if (std::getenv("WP_LOG_FS_STACK")) {
            print_call_stack();
        }
    }
    switch (command) {
    case kFsCreateDirectory: {
        int32_t result = nand::create_directory(read_string(input + kFsPathOffset, kFsPathSize));
        if (result == 0) {
            ticks += kSuperblockWriteTicks;
        }
        return result;
    }
    case kFsCreateFile:
        ticks += kSuperblockWriteTicks;
        return nand::create_file(read_string(input + kFsPathOffset, kFsPathSize));
    case kFsDelete:
        ticks += kSuperblockWriteTicks;
        return nand::remove(read_string(input, kFsPathSize));
    case kFsRename:
        ticks += kSuperblockWriteTicks;
        return nand::rename(read_string(input, kFsPathSize), read_string(input + kFsPathSize, kFsPathSize));
    case kFsGetAttributes: {
        std::string path = read_string(input, kFsPathSize);
        ticks += lookup_ticks(path, true);
        std::memset(host(output), 0, 12);
        return nand::exists(path) ? 0 : nand::kNotFound;
    }
    default:
        log_once("ioctl", "/dev/fs", command);
        return 0;
    }
}

int32_t fs_vector_command(uint32_t command, uint32_t input_count, uint32_t vectors) {
    static const bool trace = std::getenv("WP_LOG_FS") != nullptr;
    if (trace) {
        std::fprintf(stderr, "FS ioctlv %#x %s\n", command, read_string(rd32(vectors), kFsPathSize).c_str());
    }
    if (command == kFsReadDirectory) {
        std::string path = read_string(rd32(vectors), kFsPathSize);
        uint32_t names = rd32(vectors + (input_count) * kVectorSize);
        uint32_t count_address = rd32(vectors + (input_count + 1) * kVectorSize);
        uint32_t maximum = input_count > 1 ? rd32(rd32(vectors + kVectorSize)) : 0;
        uint32_t count = 0;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(nand::host_directory(path), error)) {
            if (names != 0 && (maximum == 0 || count < maximum)) {
                std::string name = entry.path().filename().string();
                name.resize(kFsNameSize - 1);
                std::memcpy(host(names + count * kFsNameSize), name.c_str(), name.size() + 1);
            }
            count++;
        }
        wr32(count_address, count);
        return 0;
    }
    if (command == kFsGetUsage) {
        std::string path = read_string(rd32(vectors), kFsPathSize);
        std::error_code error;
        std::filesystem::path directory = nand::host_directory(path);
        if (!std::filesystem::exists(directory, error)) {
            return nand::kNotFound;
        }
        if (!std::filesystem::is_directory(directory, error)) {
            return nand::kInvalid;
        }
        uint32_t clusters = 0;
        uint32_t inodes = 1;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error)) {
            inodes++;
            if (entry.is_regular_file(error)) {
                clusters += static_cast<uint32_t>((entry.file_size(error) + kFsClusterSize - 1) / kFsClusterSize);
            }
        }
        wr32(rd32(vectors + input_count * kVectorSize), clusters);
        wr32(rd32(vectors + (input_count + 1) * kVectorSize), inodes);
        return 0;
    }
    log_once("ioctlv", "/dev/fs", command);
    return 0;
}

}

void update() {
    bluetooth::update();
}

bool take_completion(uint32_t& request, int32_t& result) {
    if (!bluetooth::take_completion(request, result)) {
        return false;
    }
    wr32(request + 4, static_cast<uint32_t>(result));
    return true;
}

int32_t send(uint32_t request, uint64_t& ticks) {
    uint32_t command = rd32(request);
    int32_t descriptor = static_cast<int32_t>(rd32(request + 8));
    int32_t result = 0;
    static const bool log_requests = std::getenv("WP_LOG_IOS") != nullptr;
    if (log_requests) {
        std::fprintf(stderr, "IOS request cmd=%u fd=%d a=%08x b=%08x c=%08x d=%08x", command, descriptor, rd32(request + 12), rd32(request + 16),
                     rd32(request + 20), rd32(request + 24));
        std::fputc(10, stderr);
    }
    auto device = g_devices.find(descriptor);
    bool file = device != g_devices.end() && device->second.file >= 0;
    bool file_system = device != g_devices.end() && device->second.path == kFileSystem;
    ticks = file || file_system ? kIpcOverheadTicks : kDefaultReplyTicks;
    switch (command) {
    case kOpen: {
        std::string path = read_string(rd32(request + 12));
        if (path.rfind("/dev/", 0) != 0) {
            ticks = kIpcOverheadTicks + lookup_ticks(path, false);
        } else {
            ticks = path == kFileSystem ? kIpcOverheadTicks : kDefaultReplyTicks;
        }
        result = open(path, rd32(request + 16));
        break;
    }
    case kClose:
        if (device != g_devices.end()) {
            if (file) {
                if (g_cache_descriptor == descriptor) {
                    ticks += flush_cache();
                    g_cache_descriptor = -1;
                }
                if (device->second.superblock_flush) {
                    ticks += kSuperblockWriteTicks;
                }
            }
            if (bluetooth::handles(device->second.path)) {
                bluetooth::close();
            }
            nand::close(device->second.file);
            g_devices.erase(device);
        }
        break;
    case kRead:
        if (file) {
            ticks += read_write_ticks(descriptor, device->second, false, rd32(request + 16));
        }
        result = device != g_devices.end() ? nand::read(device->second.file, rd32(request + 12), rd32(request + 16))
                                           : nand::kInvalid;
        break;
    case kWrite:
        if (file) {
            ticks += read_write_ticks(descriptor, device->second, true, rd32(request + 16));
        }
        result = device != g_devices.end() ? nand::write(device->second.file, rd32(request + 12), rd32(request + 16))
                                           : nand::kInvalid;
        break;
    case kSeek:
        result = device != g_devices.end()
                     ? nand::seek(device->second.file, static_cast<int32_t>(rd32(request + 12)),
                                  static_cast<int32_t>(rd32(request + 16)))
                     : nand::kInvalid;
        break;
    case kIoctl:
        result = file_system ? fs_command(rd32(request + 12), rd32(request + 16), rd32(request + 24), ticks)
                             : ioctl(descriptor, rd32(request + 12), rd32(request + 16), rd32(request + 20), rd32(request + 24),
                                     rd32(request + 28));
        break;
    case kIoctlv:
        if (device != g_devices.end() && bluetooth::handles(device->second.path)) {
            result = bluetooth::ioctlv(request, rd32(request + 12), rd32(request + 16), rd32(request + 20), rd32(request + 24));
            if (result == kDeferred) {
                return result;
            }
            break;
        }
        result = ioctlv(descriptor, rd32(request + 12), rd32(request + 16), rd32(request + 20), rd32(request + 24));
        break;
    default:
        std::fprintf(stderr, "IOS request %u on %s\n", command, device_name(descriptor).c_str());
        break;
    }
    wr32(request + 4, static_cast<uint32_t>(result));
    return result;
}

int32_t open(const std::string& path, uint32_t mode) {
    Device device{path, -1};
    if (path.rfind("/dev/", 0) != 0) {
        int32_t file = nand::open(path, mode);
        if (file < 0) {
            std::fprintf(stderr, "IOS open %s failed (%d)\n", path.c_str(), file);
            log::write("ios", "open %s failed (%d)", path.c_str(), file);
            return file;
        }
        device.file = file;
    }
    std::fprintf(stderr, "IOS open %s\n", path.c_str());
    log::write("ios", "open %s", path.c_str());
    int32_t descriptor = g_next_descriptor++;
    g_devices[descriptor] = device;
    return descriptor;
}

int32_t ioctl(int32_t descriptor, uint32_t command, uint32_t input, uint32_t, uint32_t output, uint32_t) {
    auto it = g_devices.find(descriptor);
    const std::string device = it == g_devices.end() ? "?" : it->second.path;
    if (device == "/dev/di") {
        return di_command(command, input, output);
    }
    if (it != g_devices.end() && it->second.file >= 0 && command == kFsGetFileStats) {
        int32_t position = nand::seek(it->second.file, 0, 1);
        int32_t size = nand::seek(it->second.file, 0, 2);
        nand::seek(it->second.file, position, 0);
        wr32(output, static_cast<uint32_t>(size));
        wr32(output + 4, static_cast<uint32_t>(position));
        return 0;
    }
    log_once("ioctl", device, command);
    return 0;
}

int32_t ioctlv(int32_t descriptor, uint32_t command, uint32_t input_count, uint32_t output_count, uint32_t vectors) {
    const std::string device = device_name(descriptor);
    if (device == "/dev/es" && title::active()) {
        bool handled = false;
        int32_t result = title::es_ioctlv(command, input_count, output_count, vectors, handled);
        if (handled) {
            return result;
        }
    }
    if (device == "/dev/es" && command == kEsGetTitleId) {
        uint32_t output = rd32(vectors + input_count * kVectorSize);
        wr32(output, kTitleIdHigh);
        wr32(output + 4, rd32(kGameIdAddress));
        return 0;
    }
    if (device == "/dev/es" && command == kEsGetConsumption) {
        wr32(rd32(vectors + (input_count + 1) * kVectorSize), 0);
        return 0;
    }
    if (device == "/dev/es" && command == kEsGetDiscTicketView) {
        uint32_t output = rd32(vectors + input_count * kVectorSize);
        std::memset(host(output), 0, rd32(vectors + input_count * kVectorSize + 4));
        return 0;
    }
    if (device == "/dev/fs") {
        return fs_vector_command(command, input_count, vectors);
    }
    log_once("ioctlv", device, command);
    return 0;
}

}
