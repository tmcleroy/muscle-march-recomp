#include "wp/title.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "wp/log.h"
#include "wp/memory.h"

namespace wp::title {

namespace {

constexpr uint32_t kVectorSize = 8;
constexpr size_t kTmdHeaderSize = 0x1E4;
constexpr size_t kTmdContentSize = 36;
constexpr size_t kTicketViewSize = 0xD8;

constexpr uint32_t kOpenTitleContent = 0x09;
constexpr uint32_t kReadContent = 0x0A;
constexpr uint32_t kCloseContent = 0x0B;
constexpr uint32_t kGetViewCount = 0x12;
constexpr uint32_t kGetViews = 0x13;
constexpr uint32_t kGetTmdViewSize = 0x14;
constexpr uint32_t kGetTmdView = 0x15;
constexpr uint32_t kGetConsumption = 0x16;
constexpr uint32_t kGetTitleDirectory = 0x1D;
constexpr uint32_t kGetTitleId = 0x20;
constexpr uint32_t kSeekContent = 0x23;
constexpr uint32_t kOpenContent = 0x24;

constexpr int32_t kEsInvalid = -1017;
constexpr int32_t kEsNotFound = -106;

struct Content {
    uint32_t id;
    uint16_t index;
    std::vector<uint8_t> data;
};

struct OpenContent {
    size_t content;
    uint32_t position;
};

bool g_active = false;
std::string g_directory;
std::vector<uint8_t> g_tmd;
std::vector<Content> g_contents;
std::map<int32_t, OpenContent> g_open;

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

bool read_file(const std::string& path, std::vector<uint8_t>& data) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    data.resize(static_cast<size_t>(std::ftell(file)));
    std::fseek(file, 0, SEEK_SET);
    bool ok = std::fread(data.data(), 1, data.size(), file) == data.size();
    std::fclose(file);
    return ok;
}

uint32_t vector_address(uint32_t vectors, uint32_t index) {
    return rd32(vectors + index * kVectorSize);
}

uint32_t vector_size(uint32_t vectors, uint32_t index) {
    return rd32(vectors + index * kVectorSize + 4);
}

uint16_t content_count() {
    return be16(&g_tmd[0x1DE]);
}

std::vector<uint8_t> tmd_view() {
    std::vector<uint8_t> view(g_tmd.begin() + 0x180, g_tmd.begin() + 0x1D8);
    view.insert(view.end(), g_tmd.begin() + 0x1DC, g_tmd.begin() + 0x1E0);
    for (uint16_t i = 0; i < content_count(); i++) {
        const uint8_t* entry = &g_tmd[kTmdHeaderSize + i * kTmdContentSize];
        view.insert(view.end(), entry, entry + 16);
    }
    return view;
}

int32_t open_content(uint32_t index) {
    for (size_t i = 0; i < g_contents.size(); i++) {
        if (g_contents[i].index == index) {
            // IOS hands out the lowest free content descriptor (games keep the number).
            int32_t cfd = 0;
            while (g_open.count(cfd)) {
                cfd++;
            }
            g_open[cfd] = {i, 0};
            log::write("ios", "ES open content %u -> cfd %d", index, cfd);
            return cfd;
        }
    }
    return kEsNotFound;
}

}

bool mount(const std::string& extracted_directory) {
    if (!read_file(extracted_directory + "/title.tmd", g_tmd) || g_tmd.size() < kTmdHeaderSize) {
        return false;
    }
    for (uint16_t i = 0; i < content_count(); i++) {
        const uint8_t* entry = &g_tmd[kTmdHeaderSize + i * kTmdContentSize];
        Content content{be32(entry), be16(entry + 4), {}};
        char name[32];
        std::snprintf(name, sizeof name, "/contents/%02u.app", content.index);
        if (!read_file(extracted_directory + name, content.data)) {
            std::fprintf(stderr, "missing content %s\n", name);
            return false;
        }
        g_contents.push_back(std::move(content));
    }
    g_active = true;
    g_directory = extracted_directory;
    return true;
}

const std::string& directory() {
    return g_directory;
}

bool active() {
    return g_active;
}

uint32_t title_high() {
    return g_active ? be32(&g_tmd[0x18C]) : 0x00010000;
}

uint32_t title_low() {
    return g_active ? be32(&g_tmd[0x190]) : rd32(0x80000000);
}

uint16_t group_id() {
    return g_active ? be16(&g_tmd[0x198]) : 0;
}

uint32_t ios_version() {
    return g_active ? be32(&g_tmd[0x188]) : 56;
}

int32_t es_ioctlv(uint32_t command, uint32_t input_count, uint32_t output_count, uint32_t vectors, bool& handled) {
    handled = true;
    const uint32_t out = input_count;
    static const bool trace = std::getenv("WP_LOG_ES") != nullptr;
    if (trace) {
        std::fprintf(stderr, "ES ioctlv %#x in=%u:", command, input_count);
        for (uint32_t i = 0; i < input_count; i++) {
            uint32_t size = vector_size(vectors, i);
            std::fprintf(stderr, " [%u]", size);
            for (uint32_t j = 0; j < size && j < 16; j += 4) {
                std::fprintf(stderr, " %08x", rd32(vector_address(vectors, i) + j));
            }
        }
        std::fprintf(stderr, " out:");
        for (uint32_t i = out; i < out + output_count; i++) {
            std::fprintf(stderr, " [%u]", vector_size(vectors, i));
        }
        std::fputc(10, stderr);
    }
    switch (command) {
    case kGetTitleId:
        wr32(vector_address(vectors, out), title_high());
        wr32(vector_address(vectors, out) + 4, title_low());
        return 0;
    case kGetConsumption:
        if (output_count >= 2) {
            wr32(vector_address(vectors, out + 1), 0);
        }
        return 0;
    case kGetTmdViewSize:
        wr32(vector_address(vectors, out), static_cast<uint32_t>(tmd_view().size()));
        return 0;
    case kGetTmdView: {
        std::vector<uint8_t> view = tmd_view();
        uint32_t size = std::min<uint32_t>(vector_size(vectors, out), static_cast<uint32_t>(view.size()));
        std::memcpy(host(vector_address(vectors, out)), view.data(), size);
        return 0;
    }
    case kGetViewCount:
        wr32(vector_address(vectors, out), 1);
        return 0;
    case kGetViews: {
        uint32_t address = vector_address(vectors, out);
        std::memset(host(address), 0, kTicketViewSize);
        wr32(address + 0x0C, title_high());
        wr32(address + 0x10, title_low());
        return 0;
    }
    case kGetTitleDirectory: {
        char path[32];
        std::snprintf(path, sizeof path, "/title/%08x/%08x/data", title_high(), title_low());
        std::memcpy(host(vector_address(vectors, out)), path, std::strlen(path) + 1);
        return 0;
    }
    case kOpenContent:
    case kOpenTitleContent:
        // One input: index into the running title. Three: title id, ticket view, index.
        return open_content(rd32(vector_address(vectors, input_count - 1)));
    case kReadContent: {
        auto it = g_open.find(static_cast<int32_t>(rd32(vector_address(vectors, 0))));
        if (it == g_open.end()) {
            return kEsInvalid;
        }
        const std::vector<uint8_t>& data = g_contents[it->second.content].data;
        uint32_t position = it->second.position;
        uint32_t size = vector_size(vectors, out);
        uint32_t count = position < data.size() ? std::min<uint32_t>(size, static_cast<uint32_t>(data.size()) - position) : 0;
        std::memcpy(host(vector_address(vectors, out)), data.data() + position, count);
        it->second.position += count;
        return static_cast<int32_t>(count);
    }
    case kSeekContent: {
        auto it = g_open.find(static_cast<int32_t>(rd32(vector_address(vectors, 0))));
        if (it == g_open.end()) {
            return kEsInvalid;
        }
        int32_t offset = static_cast<int32_t>(rd32(vector_address(vectors, 1)));
        uint32_t whence = rd32(vector_address(vectors, 2));
        int64_t base = whence == 0 ? 0 : whence == 1 ? it->second.position : static_cast<int64_t>(g_contents[it->second.content].data.size());
        int64_t target = base + offset;
        if (target < 0 || target > static_cast<int64_t>(g_contents[it->second.content].data.size())) {
            return kEsInvalid;
        }
        it->second.position = static_cast<uint32_t>(target);
        return static_cast<int32_t>(target);
    }
    case kCloseContent:
        g_open.erase(static_cast<int32_t>(rd32(vector_address(vectors, 0))));
        return 0;
    default:
        handled = false;
        return 0;
    }
}

}
