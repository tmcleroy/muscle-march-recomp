#include "wp/storage_hle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

#include "wp/cpu.h"
#include "wp/hle.h"
#include "wp/memory.h"
#include "wp/nand.h"
#include "wp/settings.h"
#include "wp/title.h"

namespace wp::storage_hle {

namespace {

// CNT result codes (RVL SDK).
constexpr int32_t kOk = 0;
constexpr int32_t kNotFound = -5003;    // CNT_RESULT_ACCESS / not found
constexpr int32_t kBadHandle = -5000;

// Our use of the guest structures (sizes the game allocates are larger):
//   CNTHandle:   +0x1C content index, +0x24 type (1 = NAND content)
//   CNTFileInfo: +0x0 handle, +0x4 open-file id, +0x8 length, +0xC position
constexpr uint32_t kHandleIndex = 0x1C;
constexpr uint32_t kHandleType = 0x24;

bool enabled() {
    static const bool value = [] {
        std::string text = settings::text("storage.native", "WP_NATIVE_STORAGE");
        return text.empty() || std::atoi(text.c_str()) != 0;
    }();
    return value && title::active();
}

struct OpenFile {
    std::FILE* file = nullptr;
};

std::map<uint32_t, OpenFile> g_files;
uint32_t g_next_id = 1;

std::string read_string(uint32_t address) {
    std::string text;
    for (char ch; (ch = static_cast<char>(rd8(address))) != 0 && text.size() < 256; address++) {
        text.push_back(ch);
    }
    return text;
}

#define FALLBACK(name)          \
    if (!enabled()) {           \
        call_original(c, name); \
        return;                 \
    }

void cnt_init(Cpu& c) {
    FALLBACK("CNTInit");
    c.r[3] = kOk;
}

void cnt_shutdown(Cpu& c) {
    FALLBACK("CNTShutdown");
    c.r[3] = kOk;
}

// CNTInitHandle(s32 contentIndex, CNTHandle* handle, MEMAllocator* allocator)
void cnt_init_handle(Cpu& c) {
    FALLBACK("CNTInitHandle");
    uint32_t handle = c.r[4];
    wr32(handle + kHandleIndex, c.r[3]);
    wr8(handle + kHandleType, 1);
    c.r[3] = kOk;
}

void cnt_release_handle(Cpu& c) {
    FALLBACK("CNTReleaseHandle");
    c.r[3] = kOk;
}

// CNTOpen(CNTHandle* handle, const char* path, CNTFileInfo* info)
void cnt_open(Cpu& c) {
    FALLBACK("CNTOpen");
    uint32_t handle = c.r[3];
    std::string path = read_string(c.r[4]);
    uint32_t info = c.r[5];
    while (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    char index[8];
    std::snprintf(index, sizeof index, "%02u", rd32(handle + kHandleIndex));
    std::string host_path = title::directory() + "/files/" + index + "/" + path;
    std::FILE* file = std::fopen(host_path.c_str(), "rb");
    if (!file) {
        std::fprintf(stderr, "CNTOpen: %s not found\n", host_path.c_str());
        c.r[3] = static_cast<uint32_t>(kNotFound);
        return;
    }
    std::fseek(file, 0, SEEK_END);
    long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    uint32_t id = g_next_id++;
    g_files[id].file = file;
    wr32(info + 0x0, handle);
    wr32(info + 0x4, id);
    wr32(info + 0x8, static_cast<uint32_t>(length));
    wr32(info + 0xC, 0);
    c.r[3] = kOk;
}

void cnt_get_length(Cpu& c) {
    FALLBACK("CNTGetLength");
    c.r[3] = rd32(c.r[3] + 0x8);
}

// CNTRead(CNTFileInfo* info, void* buffer, u32 length): reads at the file position.
void cnt_read(Cpu& c) {
    FALLBACK("CNTRead");
    uint32_t info = c.r[3];
    auto it = g_files.find(rd32(info + 0x4));
    if (it == g_files.end()) {
        c.r[3] = static_cast<uint32_t>(kBadHandle);
        return;
    }
    uint32_t position = rd32(info + 0xC);
    uint32_t length = rd32(info + 0x8);
    uint32_t count = position < length ? std::min(c.r[5], length - position) : 0;
    std::fseek(it->second.file, position, SEEK_SET);
    size_t read = std::fread(host(c.r[4]), 1, count, it->second.file);
    wr32(info + 0xC, position + static_cast<uint32_t>(read));
    c.r[3] = static_cast<uint32_t>(read);
}

void cnt_close(Cpu& c) {
    FALLBACK("CNTClose");
    uint32_t info = c.r[3];
    auto it = g_files.find(rd32(info + 0x4));
    if (it != g_files.end()) {
        std::fclose(it->second.file);
        g_files.erase(it);
    }
    c.r[3] = kOk;
}

// --- NAND (save data) -------------------------------------------------------
// Synchronous NAND API used by the save manager and the NU engine, on top of
// the engine's host-backed NAND (wp::nand). NANDFileInfo +0 holds our handle.

constexpr int32_t kNandOk = 0;
constexpr int32_t kNandExists = -6;
constexpr int32_t kNandInvalid = -8;
constexpr int32_t kNandNoExists = -12;

int32_t nand_result(int32_t ios) {
    if (ios >= 0) {
        return ios;
    }
    switch (ios) {
    case nand::kNotFound:
        return kNandNoExists;
    case nand::kExists:
        return kNandExists;
    default:
        return kNandInvalid;
    }
}

// Relative NAND paths are relative to the title's data directory (the home dir).
std::string nand_path(uint32_t address) {
    std::string path = read_string(address);
    if (!path.empty() && path[0] == '/') {
        return path;
    }
    char home[64];
    std::snprintf(home, sizeof home, "/title/%08x/%08x/data/", title::title_high(), title::title_low());
    return home + path;
}

void trace(const char* name, uint32_t a, uint32_t b, uint32_t d, uint32_t result) {
    static const bool enabled = std::getenv("WP_LOG_NAND") != nullptr;
    if (enabled) {
        std::fprintf(stderr, "NAND %s(%08x, %08x, %08x) = %d\n", name, a, b, d, static_cast<int32_t>(result));
        if (std::getenv("WP_LOG_NAND_STACK")) {
            print_call_stack();
        }
    }
}

#define NAND_HANDLER(fn, name)                                      \
    void fn##_impl(Cpu& c);                                         \
    void fn(Cpu& c) {                                               \
        if (!enabled()) {                                           \
            call_original(c, name);                                 \
            return;                                                 \
        }                                                           \
        uint32_t a = c.r[3], b = c.r[4], d = c.r[5];                 \
        fn##_impl(c);                                               \
        trace(name, a, b, d, c.r[3]);                               \
    }                                                               \
    void fn##_impl(Cpu& c)

NAND_HANDLER(nand_open, "NANDOpen") {
    int32_t handle = nand::open(nand_path(c.r[3]), c.r[5]);
    if (handle >= 0) {
        wr32(c.r[4], static_cast<uint32_t>(handle));
    }
    c.r[3] = static_cast<uint32_t>(handle >= 0 ? kNandOk : nand_result(handle));
}

// NANDSimpleSafeOpen(path, info, mode, work, size): like the SDK, work on a copy
// (/tmp/safeN) and replace the original only on NANDSimpleSafeClose.
struct SafeFile {
    std::string original;
    std::string temporary;
};

std::map<int32_t, SafeFile> g_safe;
int g_safe_serial = 0;

bool copy_nand_file(const std::string& from, const std::string& to) {
    int32_t in = nand::open(from, 1);
    if (in < 0) {
        return false;
    }
    nand::remove(to);
    nand::create_file(to);
    int32_t out = nand::open(to, 2);
    std::FILE* src = std::fopen(nand::host_directory(from).c_str(), "rb");
    std::FILE* dst = std::fopen(nand::host_directory(to).c_str(), "wb");
    if (src && dst) {
        char buffer[16384];
        size_t count;
        while ((count = std::fread(buffer, 1, sizeof buffer, src)) > 0) {
            std::fwrite(buffer, 1, count, dst);
        }
    }
    if (src) {
        std::fclose(src);
    }
    if (dst) {
        std::fclose(dst);
    }
    nand::close(in);
    nand::close(out);
    return true;
}

NAND_HANDLER(nand_safe_open, "NANDSimpleSafeOpen") {
    std::string path = nand_path(c.r[3]);
    uint32_t mode = c.r[5];
    if (!nand::exists(path)) {
        c.r[3] = static_cast<uint32_t>(kNandNoExists);
        return;
    }
    std::string target = path;
    if (mode & 2) {
        nand::create_directory("/tmp");
        target = "/tmp/safe" + std::to_string(g_safe_serial++);
        copy_nand_file(path, target);
    }
    int32_t handle = nand::open(target, mode);
    if (handle < 0) {
        c.r[3] = static_cast<uint32_t>(nand_result(handle));
        return;
    }
    wr32(c.r[4], static_cast<uint32_t>(handle));
    if (mode & 2) {
        g_safe[handle] = SafeFile{path, target};
    }
    c.r[3] = kNandOk;
}

NAND_HANDLER(nand_close, "NANDClose") {
    nand::close(static_cast<int32_t>(rd32(c.r[3])));
    c.r[3] = kNandOk;
}

NAND_HANDLER(nand_safe_cancel, "NANDSimpleSafeCancel") {
    int32_t handle = static_cast<int32_t>(rd32(c.r[3]));
    nand::close(handle);
    auto it = g_safe.find(handle);
    if (it != g_safe.end()) {
        nand::remove(it->second.temporary);
        g_safe.erase(it);
    }
    c.r[3] = kNandOk;
}

NAND_HANDLER(nand_safe_close, "NANDSimpleSafeClose") {
    int32_t handle = static_cast<int32_t>(rd32(c.r[3]));
    nand::close(handle);
    auto it = g_safe.find(handle);
    if (it != g_safe.end()) {
        nand::remove(it->second.original);
        nand::rename(it->second.temporary, it->second.original);
        g_safe.erase(it);
    }
    c.r[3] = kNandOk;
}

NAND_HANDLER(nand_read, "NANDRead") {
    c.r[3] = static_cast<uint32_t>(nand_result(nand::read(static_cast<int32_t>(rd32(c.r[3])), c.r[4], c.r[5])));
}

NAND_HANDLER(nand_write, "NANDWrite") {
    c.r[3] = static_cast<uint32_t>(nand_result(nand::write(static_cast<int32_t>(rd32(c.r[3])), c.r[4], c.r[5])));
}

NAND_HANDLER(nand_seek, "NANDSeek") {
    c.r[3] = static_cast<uint32_t>(nand_result(
        nand::seek(static_cast<int32_t>(rd32(c.r[3])), static_cast<int32_t>(c.r[4]), static_cast<int32_t>(c.r[5]))));
}

NAND_HANDLER(nand_get_length, "NANDGetLength") {
    int32_t handle = static_cast<int32_t>(rd32(c.r[3]));
    int32_t position = nand::seek(handle, 0, 1);
    int32_t length = nand::seek(handle, 0, 2);
    nand::seek(handle, position, 0);
    if (length < 0) {
        c.r[3] = static_cast<uint32_t>(nand_result(length));
        return;
    }
    wr32(c.r[4], static_cast<uint32_t>(length));
    c.r[3] = kNandOk;
}

// NANDGetStatus(path, NANDStatus*): {u32 ownerId, u16 groupId, u8 attribute, u8 permission}
NAND_HANDLER(nand_get_status, "NANDGetStatus") {
    std::string path = nand_path(c.r[3]);
    if (!nand::exists(path)) {
        c.r[3] = static_cast<uint32_t>(kNandNoExists);
        return;
    }
    if (c.r[4]) {
        wr32(c.r[4], 0);
        wr16(c.r[4] + 4, 0);
        wr8(c.r[4] + 6, 0);
        wr8(c.r[4] + 7, 0x3F);
    }
    c.r[3] = kNandOk;
}

NAND_HANDLER(nand_create, "NANDCreate") {
    c.r[3] = static_cast<uint32_t>(nand_result(nand::create_file(nand_path(c.r[3]))));
}

NAND_HANDLER(nand_create_dir, "NANDCreateDir") {
    c.r[3] = static_cast<uint32_t>(nand_result(nand::create_directory(nand_path(c.r[3]))));
}

// NANDReadDir(path, char* names, u32* count): only the count is needed by this game.
NAND_HANDLER(nand_read_dir, "NANDReadDir") {
    std::string path = nand_path(c.r[3]);
    if (!nand::exists(path)) {
        c.r[3] = static_cast<uint32_t>(kNandNoExists);
        return;
    }
    if (c.r[5]) {
        wr32(c.r[5], 0);
    }
    c.r[3] = kNandOk;
}

// NANDMove(path, destDir)
NAND_HANDLER(nand_move, "NANDMove") {
    std::string from = nand_path(c.r[3]);
    std::string to = nand_path(c.r[4]);
    size_t slash = from.find_last_of('/');
    to += (to.empty() || to.back() != '/' ? "/" : "") + from.substr(slash + 1);
    nand::remove(to);
    c.r[3] = static_cast<uint32_t>(nand_result(nand::rename(from, to)));
}

NAND_HANDLER(nand_delete, "NANDDelete") {
    c.r[3] = static_cast<uint32_t>(nand_result(nand::remove(nand_path(c.r[3]))));
}

// NANDGetType(path, u8* type): 1 = file, 2 = directory.
NAND_HANDLER(nand_get_type, "NANDGetType") {
    std::string path = nand_path(c.r[3]);
    if (!nand::exists(path)) {
        c.r[3] = static_cast<uint32_t>(kNandNoExists);
        return;
    }
    if (c.r[4]) {
        wr8(c.r[4], std::filesystem::is_directory(nand::host_directory(path)) ? 2 : 1);
    }
    c.r[3] = kNandOk;
}

// NANDCheck(fsBlock, inode, u32* answer): always enough space.
NAND_HANDLER(nand_check, "NANDCheck") {
    wr32(c.r[5], 0);
    c.r[3] = kNandOk;
}

NAND_HANDLER(nand_get_available_area, "NANDGetAvailableArea") {
    if (c.r[3]) {
        wr32(c.r[3], 0x8000);
    }
    if (c.r[4]) {
        wr32(c.r[4], 0x1000);
    }
    c.r[3] = kNandOk;
}

}  // namespace

const input_hle::Handler kHandlers[] = {
    {"CNTInit", cnt_init},
    {"CNTShutdown", cnt_shutdown},
    {"CNTInitHandle", cnt_init_handle},
    {"CNTReleaseHandle", cnt_release_handle},
    {"CNTOpen", cnt_open},
    {"CNTGetLength", cnt_get_length},
    {"CNTRead", cnt_read},
    {"CNTClose", cnt_close},
    {"NANDOpen", nand_open},
    {"NANDSimpleSafeOpen", nand_safe_open},
    {"NANDClose", nand_close},
    {"NANDSimpleSafeClose", nand_safe_close},
    {"NANDSimpleSafeCancel", nand_safe_cancel},
    {"NANDRead", nand_read},
    {"NANDWrite", nand_write},
    {"NANDSeek", nand_seek},
    {"NANDGetLength", nand_get_length},
    {"NANDGetStatus", nand_get_status},
    {"NANDCreate", nand_create},
    {"NANDCreateDir", nand_create_dir},
    {"NANDReadDir", nand_read_dir},
    {"NANDMove", nand_move},
    {"NANDCheck", nand_check},
    {"NANDDelete", nand_delete},
    {"NANDGetType", nand_get_type},
    {"NANDGetAvailableArea", nand_get_available_area},
    {nullptr, nullptr},
};

}
