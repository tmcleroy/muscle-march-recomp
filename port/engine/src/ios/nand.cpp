#include "wp/nand.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <map>
#include <vector>

#include "wp/log.h"
#include "wp/memory.h"
#include "wp/save_backup.h"
#include "wp/settings.h"

namespace wp::nand {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kModeRead = 1;
constexpr uint32_t kModeWrite = 2;
constexpr size_t kSysconfSize = 0x4000;
constexpr uint8_t kTypeBigArray = 1;
constexpr uint8_t kTypeSmallArray = 2;
constexpr uint8_t kTypeByte = 3;
constexpr uint8_t kTypeLong = 5;
constexpr uint8_t kTypeBool = 7;
constexpr uint8_t kLanguageEnglish = 1;
constexpr uint8_t kLanguageGerman = 2;
constexpr uint8_t kLanguageFrench = 3;
constexpr uint8_t kLanguageSpanish = 4;
constexpr uint8_t kLanguageItalian = 5;
constexpr uint8_t kLanguageDutch = 6;
constexpr uint32_t kGameIdAddress = 0x80000000;
constexpr int kDefaultBackups = 5;
constexpr auto kBackupInterval = std::chrono::minutes(1);

struct OpenFile {
    std::FILE* file;
    fs::path path;
    bool written;
};

fs::path g_root;
std::map<int32_t, OpenFile> g_files;
int32_t g_next_handle = 1;
fs::path g_save;
fs::path g_backups;
bool g_backup_pending = false;
bool g_backed_up = false;
std::chrono::steady_clock::time_point g_last_backup;

fs::path host_path(const std::string& path) {
    fs::path relative;
    for (const auto& part : fs::path(path).relative_path()) {
        if (part != "." && part != "..") {
            relative /= part;
        }
    }
    return g_root / relative;
}

int backups_to_keep() {
    const std::string value = settings::text("saves.backups", "WP_SAVE_BACKUPS");
    char* end = nullptr;
    long number = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || *end != 0) {
        return kDefaultBackups;
    }
    return static_cast<int>(std::clamp(number, 0L, 1000L));
}

void back_up_save(const char* moment) {
    int keep = backups_to_keep();
    if (keep == 0 || g_save.empty()) {
        return;
    }
    g_backup_pending = false;
    saves::Outcome outcome = saves::back_up(g_root, g_save, g_backups, keep, std::time(nullptr));
    if (outcome.result != saves::Result::NoSave && outcome.result != saves::Result::Unchanged) {
        g_backed_up = true;
        g_last_backup = std::chrono::steady_clock::now();
    }
    if (outcome.result == saves::Result::Invalid) {
        std::fprintf(stderr, "save backup (%s): %s; no backup was made and the existing backups are kept. Restore one from the launcher's Saves page if the game reports damaged data.\n",
                     moment, outcome.message.c_str());
    } else if (outcome.result == saves::Result::Failed) {
        std::fprintf(stderr, "save backup (%s) failed: %s\n", moment, outcome.message.c_str());
    } else if (outcome.result == saves::Result::Created) {
        std::fprintf(stderr, "save backup (%s): %s\n", moment, outcome.message.c_str());
    }
    log::write("nand", "save backup (%s): %s", moment, outcome.message.c_str());
}

bool same_element(const fs::path& first, const fs::path& second) {
    const auto& a = first.native();
    const auto& b = second.native();
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](auto x, auto y) {
               return std::towlower(static_cast<wint_t>(x)) == std::towlower(static_cast<wint_t>(y));
           });
}

bool inside_save(const fs::path& path) {
    if (g_save.empty()) {
        return false;
    }
    fs::path folder = g_root / g_save;
    auto part = path.begin();
    for (const auto& element : folder) {
        if (part == path.end() || !same_element(*part, element)) {
            return false;
        }
        ++part;
    }
    return true;
}

void save_changed() {
    if (backups_to_keep() == 0) {
        return;
    }
    bool writing = std::any_of(g_files.begin(), g_files.end(), [](const auto& entry) { return entry.second.written && inside_save(entry.second.path); });
    if (writing || (g_backed_up && std::chrono::steady_clock::now() - g_last_backup < kBackupInterval)) {
        g_backup_pending = true;
        return;
    }
    back_up_save("after a write");
}

void append16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void add_item(std::vector<std::vector<uint8_t>>& items, uint8_t type, const std::string& name,
              const std::vector<uint8_t>& data) {
    std::vector<uint8_t> item;
    item.push_back(static_cast<uint8_t>((type << 5) | (name.size() - 1)));
    item.insert(item.end(), name.begin(), name.end());
    if (type == kTypeBigArray) {
        append16(item, static_cast<uint16_t>(data.size() - 1));
    } else if (type == kTypeSmallArray) {
        item.push_back(static_cast<uint8_t>(data.size() - 1));
    }
    item.insert(item.end(), data.begin(), data.end());
    items.push_back(item);
}

std::vector<uint8_t> paired_remotes() {
    constexpr size_t kDeviceSize = 6 + 0x40;
    constexpr size_t kRegistered = 10;
    constexpr size_t kActive = 5;
    constexpr uint8_t kRemotes = 4;
    static const char kName[] = "Nintendo RVL-CNT-01";
    std::vector<uint8_t> data(1 + kDeviceSize * (kRegistered + kActive + 1), 0);
    data[0] = kRemotes;
    for (uint8_t i = 0; i < kRemotes; i++) {
        const uint8_t address[6] = {i, 0x00, 0x79, 0x19, 0x02, 0x11};
        for (size_t slot : {1 + kDeviceSize * i, 1 + kDeviceSize * (kRegistered + i)}) {
            std::memcpy(&data[slot], address, 6);
            std::memcpy(&data[slot + 6], kName, sizeof(kName) - 1);
        }
    }
    return data;
}

uint8_t pal60() {
    return settings::flag("system.pal60", "WP_PAL60") ? 1 : 0;
}

struct Area {
    std::vector<uint8_t> area;
    std::vector<uint8_t> code;
};

Area console_area() {
    switch (static_cast<char>(rd8(kGameIdAddress + 3))) {
    case 'E':
        return {{'U', 'S', 'A', 0}, {'L', 'U', 0, 0}};
    case 'J':
        return {{'J', 'P', 'N', 0}, {'L', 'J', 0, 0}};
    case 'K':
        return {{'K', 'O', 'R', 0}, {'L', 'K', 'H', 0}};
    default:
        return {{'E', 'U', 'R', 0}, {'L', 'E', 'H', 0}};
    }
}

uint8_t console_language() {
    struct Name {
        const char* code;
        uint8_t language;
    };
    static const Name kNames[] = {{"en", kLanguageEnglish}, {"de", kLanguageGerman}, {"fr", kLanguageFrench},
                                  {"es", kLanguageSpanish}, {"it", kLanguageItalian}, {"nl", kLanguageDutch}};
    std::string setting = settings::text("system.language", "WP_LANGUAGE");
    for (const Name& name : kNames) {
        if (setting == name.code) {
            return name.language;
        }
    }
#ifdef _WIN32
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_GERMAN:
        return kLanguageGerman;
    case LANG_FRENCH:
        return kLanguageFrench;
    case LANG_SPANISH:
        return kLanguageSpanish;
    case LANG_ITALIAN:
        return kLanguageItalian;
    case LANG_DUTCH:
        return kLanguageDutch;
    default:
        return kLanguageEnglish;
    }
#else
    const char* locale = std::getenv("LC_ALL");
    if (!locale || !*locale) {
        locale = std::getenv("LANG");
    }
    std::string code = locale ? std::string(locale).substr(0, 2) : "";
    for (const Name& name : kNames) {
        if (code == name.code) {
            return name.language;
        }
    }
    return kLanguageEnglish;
#endif
}

std::vector<uint8_t> default_sysconf() {
    std::vector<std::vector<uint8_t>> items;
    add_item(items, kTypeBigArray, "BT.DINF", paired_remotes());
    add_item(items, kTypeBigArray, "BT.CDIF", std::vector<uint8_t>(0x205, 0));
    add_item(items, kTypeLong, "BT.SENS", {0, 0, 0, 3});
    add_item(items, kTypeByte, "BT.BAR", {1});
    add_item(items, kTypeByte, "BT.SPKV", {0x58});
    add_item(items, kTypeByte, "BT.MOT", {1});
    add_item(items, kTypeSmallArray, "IPL.NIK", {0, 'w', 0, 'i', 0, 'i', 0, 'p', 0, 'a', 0, 'r', 0, 't', 0, 'y'});
    add_item(items, kTypeByte, "IPL.LNG", {console_language()});
    std::vector<uint8_t> address(0x1008, 0);
    address[0] = 0x6c;
    add_item(items, kTypeBigArray, "IPL.SADR", address);
    std::vector<uint8_t> parental(0x4A, 0);
    parental[1] = 0x04;
    parental[2] = 0x14;
    add_item(items, kTypeSmallArray, "IPL.PC", parental);
    add_item(items, kTypeLong, "IPL.CB", {0, 0, 0, 0});
    add_item(items, kTypeByte, "IPL.AR", {1});
    add_item(items, kTypeByte, "IPL.SSV", {1});
    add_item(items, kTypeBool, "IPL.CD", {0});
    add_item(items, kTypeBool, "IPL.CD2", {0});
    add_item(items, kTypeBool, "IPL.EULA", {1});
    add_item(items, kTypeByte, "IPL.UPT", {2});
    add_item(items, kTypeByte, "IPL.PGS", {0});
    add_item(items, kTypeByte, "IPL.E60", {pal60()});
    add_item(items, kTypeByte, "IPL.DH", {0});
    add_item(items, kTypeLong, "IPL.INC", {0, 0, 0, 8});
    add_item(items, kTypeLong, "IPL.FRC", {0, 0, 0, 0x28});
    add_item(items, kTypeSmallArray, "IPL.IDL", {0, 1});
    add_item(items, kTypeByte, "IPL.SND", {1});
    Area area = console_area();
    add_item(items, kTypeSmallArray, "IPL.AREA", area.area);
    add_item(items, kTypeSmallArray, "IPL.CODE", area.code);
    add_item(items, kTypeLong, "NET.WCFG", {0, 0, 0, 1});
    add_item(items, kTypeLong, "NET.CTPC", {0, 0, 0, 0});
    add_item(items, kTypeByte, "WWW.RST", {0});
    add_item(items, kTypeBool, "MPLS.MOVIE", {1});

    std::vector<uint8_t> file;
    file.push_back('S');
    file.push_back('C');
    file.push_back('v');
    file.push_back('0');
    append16(file, static_cast<uint16_t>(items.size()));
    size_t position = 4 + 2 + 2 * (items.size() + 1);
    for (const auto& item : items) {
        append16(file, static_cast<uint16_t>(position));
        position += item.size();
    }
    append16(file, static_cast<uint16_t>(position));
    for (const auto& item : items) {
        file.insert(file.end(), item.begin(), item.end());
    }
    file.resize(kSysconfSize, 0);
    file[kSysconfSize - 4] = 'S';
    file[kSysconfSize - 3] = 'C';
    file[kSysconfSize - 2] = 'e';
    file[kSysconfSize - 1] = 'd';
    return file;
}

uint16_t read16(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

bool valid_sysconf(const fs::path& path) {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (!file) {
        return false;
    }
    std::vector<uint8_t> data(kSysconfSize);
    size_t count = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (count != kSysconfSize || std::memcmp(data.data(), "SCv0", 4) != 0 || std::memcmp(&data[kSysconfSize - 4], "SCed", 4) != 0) {
        return false;
    }
    size_t entries = read16(data, 4);
    size_t table_end = 6 + 2 * (entries + 1);
    if (table_end > kSysconfSize - 4) {
        return false;
    }
    size_t previous = table_end;
    for (size_t i = 0; i <= entries; i++) {
        size_t offset = read16(data, 6 + 2 * i);
        if (offset < previous || offset > kSysconfSize - 4) {
            return false;
        }
        previous = offset;
    }
    for (const char* name : {"BT.DINF", "BT.CDIF", "IPL.SADR"}) {
        if (std::search(data.begin(), data.end(), name, name + std::strlen(name)) == data.end()) {
            return false;
        }
    }
    return true;
}

void set_byte_item(std::vector<uint8_t>& data, size_t count, const char* name, uint8_t value, std::FILE* file) {
    size_t length = std::strlen(name);
    for (size_t i = 1; i + length < count; i++) {
        if (data[i - 1] == ((kTypeByte << 5) | (length - 1)) && std::memcmp(&data[i], name, length) == 0) {
            if (data[i + length] != value) {
                std::fseek(file, static_cast<long>(i + length), SEEK_SET);
                std::fwrite(&value, 1, 1, file);
            }
            return;
        }
    }
}

void set_array_item(std::vector<uint8_t>& data, size_t count, const char* name, const std::vector<uint8_t>& value, std::FILE* file) {
    size_t length = std::strlen(name);
    for (size_t i = 1; i + length + 1 + value.size() <= count; i++) {
        if (data[i - 1] == ((kTypeSmallArray << 5) | (length - 1)) && std::memcmp(&data[i], name, length) == 0) {
            size_t start = i + length + 1;
            if (data[i + length] + 1u == value.size() && std::memcmp(&data[start], value.data(), value.size()) != 0) {
                std::fseek(file, static_cast<long>(start), SEEK_SET);
                std::fwrite(value.data(), 1, value.size(), file);
            }
            return;
        }
    }
}

void apply_settings(const fs::path& path) {
    std::FILE* file = std::fopen(path.string().c_str(), "r+b");
    if (!file) {
        return;
    }
    std::vector<uint8_t> data(kSysconfSize);
    size_t count = std::fread(data.data(), 1, data.size(), file);
    set_byte_item(data, count, "IPL.LNG", console_language(), file);
    set_byte_item(data, count, "IPL.E60", pal60(), file);
    Area area = console_area();
    set_array_item(data, count, "IPL.AREA", area.area, file);
    set_array_item(data, count, "IPL.CODE", area.code, file);
    std::fclose(file);
}

void ensure_sysconf() {
    fs::path path = host_path("/shared2/sys/SYSCONF");
    if (fs::exists(path) && valid_sysconf(path)) {
        apply_settings(path);
        return;
    }
    fs::create_directories(path.parent_path());
    std::vector<uint8_t> data = default_sysconf();
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file) {
        std::fwrite(data.data(), 1, data.size(), file);
        std::fclose(file);
    }
}

}

bool mount(const std::string& root) {
    g_root = root;
    std::error_code error;
    fs::create_directories(g_root, error);
    if (error) {
        return false;
    }
    ensure_sysconf();
    g_save = saves::save_relative(rd32(kGameIdAddress));
    g_backups = saves::backups_beside(g_root);
    g_backed_up = false;
    back_up_save("start");
    return true;
}

std::string host_directory(const std::string& path) {
    return host_path(path).string();
}

bool widescreen() {
    std::FILE* file = std::fopen(host_path("/shared2/sys/SYSCONF").string().c_str(), "rb");
    if (!file) {
        return false;
    }
    std::vector<uint8_t> data(kSysconfSize);
    size_t count = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    static const char kName[] = "IPL.AR";
    constexpr size_t kNameLength = sizeof(kName) - 1;
    for (size_t i = 0; i + kNameLength < count; i++) {
        if (std::memcmp(&data[i], kName, kNameLength) == 0) {
            return data[i + kNameLength] == 1;
        }
    }
    return false;
}

bool exists(const std::string& path) {
    return fs::exists(host_path(path));
}

int32_t open(const std::string& path, uint32_t mode) {
    fs::path target = host_path(path);
    if (!fs::is_regular_file(target)) {
        return kNotFound;
    }
    const char* flags = (mode & kModeWrite) ? "r+b" : "rb";
    std::FILE* file = std::fopen(target.string().c_str(), flags);
    if (!file) {
        return kNotFound;
    }
    int32_t handle = g_next_handle++;
    g_files[handle] = OpenFile{file, target, false};
    return handle;
}

int32_t read(int32_t handle, uint32_t destination, uint32_t length) {
    auto it = g_files.find(handle);
    if (it == g_files.end()) {
        return kInvalid;
    }
    return static_cast<int32_t>(std::fread(host(destination), 1, length, it->second.file));
}

int32_t write(int32_t handle, uint32_t source, uint32_t length) {
    auto it = g_files.find(handle);
    if (it == g_files.end()) {
        return kInvalid;
    }
    int32_t written = static_cast<int32_t>(std::fwrite(host(source), 1, length, it->second.file));
    std::fflush(it->second.file);
    it->second.written = true;
    return written;
}

int32_t seek(int32_t handle, int32_t offset, int32_t whence) {
    auto it = g_files.find(handle);
    if (it == g_files.end()) {
        return kInvalid;
    }
    static const int kOrigins[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    if (whence < 0 || whence > 2 || std::fseek(it->second.file, offset, kOrigins[whence]) != 0) {
        return kInvalid;
    }
    return static_cast<int32_t>(std::ftell(it->second.file));
}

void close(int32_t handle) {
    auto it = g_files.find(handle);
    if (it == g_files.end()) {
        return;
    }
    std::fclose(it->second.file);
    bool saved = it->second.written && inside_save(it->second.path);
    g_files.erase(it);
    if (saved || g_backup_pending) {
        save_changed();
    }
}

int32_t create_file(const std::string& path) {
    fs::path target = host_path(path);
    if (fs::exists(target)) {
        return kExists;
    }
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    std::FILE* file = std::fopen(target.string().c_str(), "wb");
    if (!file) {
        return kInvalid;
    }
    std::fclose(file);
    return 0;
}

int32_t create_directory(const std::string& path) {
    fs::path target = host_path(path);
    if (fs::exists(target)) {
        return kExists;
    }
    std::error_code error;
    fs::create_directories(target, error);
    return error ? kInvalid : 0;
}

int32_t remove(const std::string& path) {
    std::error_code error;
    return fs::remove_all(host_path(path), error) > 0 ? 0 : kNotFound;
}

int32_t rename(const std::string& from, const std::string& to) {
    std::error_code error;
    fs::path target = host_path(to);
    fs::rename(host_path(from), target, error);
    if (error) {
        return kNotFound;
    }
    if (inside_save(target)) {
        save_changed();
    }
    return 0;
}

}
