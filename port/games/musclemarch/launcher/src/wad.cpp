#include "wad.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "crypto.h"

namespace {

// The USA release (tools/extract.py knows the same file).
constexpr const char* kKnownSha1 = "ed99ce114d9793dbf21d5c9589d7947afebd4259";
constexpr const char* kTitleId = "00010001574d4d45";  // WMME

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t align64(uint32_t x) {
    return (x + 0x3F) & ~0x3Fu;
}

}  // namespace

WadCheck check_wad(const std::string& path) {
    WadCheck check;
    std::FILE* file = std::fopen(path.c_str(), "rb");  // read only, always
    if (!file) {
        check.message = "The file can't be opened.";
        return check;
    }
    std::vector<uint8_t> head(0x4000);
    size_t got = std::fread(head.data(), 1, head.size(), file);
    head.resize(got);
    Sha1 sha;
    sha.update(head.data(), head.size());
    check.size = got;
    std::vector<uint8_t> buffer(1 << 20);
    while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
        sha.update(buffer.data(), got);
        check.size += got;
    }
    std::fclose(file);
    check.sha1 = sha.hex();

    // WAD header: size 0x20, type "Is"/"ib", then certificates, ticket, TMD (64-byte aligned).
    if (head.size() < 0x20 || be32(head.data()) != 0x20 || !(std::memcmp(head.data() + 4, "Is", 2) == 0 || std::memcmp(head.data() + 4, "ib", 2) == 0)) {
        check.result = WadCheck::Result::NotWad;
        check.message = "This isn't a WiiWare WAD file.";
        return check;
    }
    uint32_t ticket = align64(align64(0x20) + be32(head.data() + 0x08));
    if (ticket + 0x1E4 > head.size()) {
        check.result = WadCheck::Result::NotWad;
        check.message = "This WAD file looks damaged.";
        return check;
    }
    char id[17];
    std::snprintf(id, sizeof id, "%08x%08x", be32(head.data() + ticket + 0x1DC), be32(head.data() + ticket + 0x1E0));
    check.title_id = id;
    if (check.title_id != kTitleId) {
        check.result = WadCheck::Result::NotMuscleMarch;
        check.message = "This WAD is a different game (title " + check.title_id + ").";
        return check;
    }
    if (check.sha1 != kKnownSha1) {
        check.result = WadCheck::Result::OtherRevision;
        check.message = "Muscle March, but not the known USA release: it may not work.";
        return check;
    }
    check.result = WadCheck::Result::Ok;
    check.message = "Muscle March (USA, WiiWare), verified.";
    return check;
}
