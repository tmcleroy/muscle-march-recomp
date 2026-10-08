#include "install.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "crypto.h"

namespace fs = std::filesystem;

namespace {

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t be64(const uint8_t* p) {
    return (uint64_t(be32(p)) << 32) | be32(p + 4);
}

size_t align(size_t x, size_t a) {
    return (x + a - 1) & ~(a - 1);
}

bool read_all(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary);  // read only, always
    if (!file) {
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool write_all(const fs::path& path, const uint8_t* data, size_t size) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(file);
}

struct Content {
    uint32_t id = 0;
    uint16_t index = 0;
    uint16_t type = 0;
    uint64_t size = 0;
    std::array<uint8_t, 20> sha1{};
    size_t offset = 0;  // of its encrypted data in the WAD
};

// The WAD's layout: header, certificates, ticket, TMD, contents, footer (each 64-byte aligned).
struct Wad {
    std::vector<uint8_t> data;
    size_t ticket = 0;
    size_t tmd = 0;
    size_t tmd_size = 0;
    std::vector<Content> contents;

    bool parse(std::string& error) {
        if (data.size() < 0x20 || be32(data.data()) != 0x20) {
            error = "This isn't a WiiWare WAD file.";
            return false;
        }
        uint32_t certificates = be32(&data[0x08]);
        uint32_t ticket_size = be32(&data[0x10]);
        tmd_size = be32(&data[0x14]);
        ticket = align(align(0x20, 64) + certificates, 64);
        tmd = align(ticket + ticket_size, 64);
        size_t offset = align(tmd + tmd_size, 64);
        if (ticket + 0x2A4 > data.size() || tmd + 0x1E4 > data.size()) {
            error = "This WAD file looks damaged.";
            return false;
        }
        uint16_t count = be16(&data[tmd + 0x1DE]);
        if (tmd + 0x1E4 + 36 * size_t(count) > data.size()) {
            error = "This WAD file looks damaged.";
            return false;
        }
        for (uint16_t i = 0; i < count; i++) {
            const uint8_t* entry = &data[tmd + 0x1E4 + 36 * size_t(i)];
            Content content;
            content.id = be32(entry);
            content.index = be16(entry + 4);
            content.type = be16(entry + 6);
            content.size = be64(entry + 8);
            std::memcpy(content.sha1.data(), entry + 16, 20);
            content.offset = offset;
            offset = align(offset + align(content.size, 16), 64);
            if (offset > data.size()) {
                error = "This WAD file is incomplete.";
                return false;
            }
            contents.push_back(content);
        }
        return true;
    }

    // The title key, decrypted with a candidate common key (IV: the title ID, then zeros).
    CommonKey title_key(const CommonKey& common) const {
        CommonKey key;
        std::memcpy(key.data(), &data[ticket + 0x1BF], 16);
        uint8_t iv[16] = {};
        std::memcpy(iv, &data[ticket + 0x1DC], 8);
        Aes128(common.data()).decrypt_cbc(key.data(), 16, iv);
        return key;
    }

    // A content decrypted (IV: its index, then zeros) and cut to size; empty if its SHA-1 is wrong.
    std::vector<uint8_t> decrypt(const Content& content, const Aes128& title) const {
        std::vector<uint8_t> out(data.begin() + static_cast<long>(content.offset),
                                 data.begin() + static_cast<long>(content.offset + align(content.size, 16)));
        uint8_t iv[16] = {static_cast<uint8_t>(content.index >> 8), static_cast<uint8_t>(content.index)};
        title.decrypt_cbc(out.data(), out.size(), iv);
        out.resize(content.size);
        Sha1 sha;
        sha.update(out.data(), out.size());
        if (sha.digest() != content.sha1) {
            out.clear();
        }
        return out;
    }

    const Content* smallest() const {
        const Content* best = nullptr;
        for (const Content& content : contents) {
            if (!best || content.size < best->size) {
                best = &content;
            }
        }
        return best;
    }
};

// Nintendo LZ10 / LZ11 (the game program is LZ11-compressed).
bool looks_lz(const std::vector<uint8_t>& d) {
    if (d.size() < 8 || (d[0] != 0x10 && d[0] != 0x11)) {
        return false;
    }
    size_t size = d[1] | (d[2] << 8) | (d[3] << 16);
    return size == 0 || (d.size() < size * 9 / 8 + 16 && size >= d.size() / 2);
}

bool lz_decompress(const std::vector<uint8_t>& d, std::vector<uint8_t>& out) {
    uint8_t kind = d[0];
    size_t size = d[1] | (d[2] << 8) | (d[3] << 16);
    size_t pos = 4;
    if (size == 0) {
        if (d.size() < 8) {
            return false;
        }
        size = d[4] | (d[5] << 8) | (d[6] << 16) | (size_t(d[7]) << 24);
        pos = 8;
    }
    out.clear();
    out.reserve(size);
    auto need = [&](size_t n) { return pos + n <= d.size(); };
    while (out.size() < size) {
        if (!need(1)) {
            return false;
        }
        uint8_t flags = d[pos++];
        for (int bit = 0; bit < 8 && out.size() < size; bit++) {
            if (!(flags & (0x80 >> bit))) {
                if (!need(1)) {
                    return false;
                }
                out.push_back(d[pos++]);
                continue;
            }
            size_t n = 0;
            size_t disp = 0;
            if (kind == 0x10) {
                if (!need(2)) {
                    return false;
                }
                n = (d[pos] >> 4) + 3;
                disp = (((d[pos] & 0xF) << 8) | d[pos + 1]) + 1;
                pos += 2;
            } else {
                if (!need(1)) {
                    return false;
                }
                uint8_t b0 = d[pos];
                int indicator = b0 >> 4;
                if (indicator == 0) {
                    if (!need(3)) {
                        return false;
                    }
                    n = (((b0 & 0xF) << 4) | (d[pos + 1] >> 4)) + 0x11;
                    disp = (((d[pos + 1] & 0xF) << 8) | d[pos + 2]) + 1;
                    pos += 3;
                } else if (indicator == 1) {
                    if (!need(4)) {
                        return false;
                    }
                    n = (((b0 & 0xF) << 12) | (d[pos + 1] << 4) | (d[pos + 2] >> 4)) + 0x111;
                    disp = (((d[pos + 2] & 0xF) << 8) | d[pos + 3]) + 1;
                    pos += 4;
                } else {
                    if (!need(2)) {
                        return false;
                    }
                    n = indicator + 1;
                    disp = (((b0 & 0xF) << 8) | d[pos + 1]) + 1;
                    pos += 2;
                }
            }
            if (disp > out.size()) {
                return false;
            }
            for (size_t i = 0; i < n; i++) {
                out.push_back(out[out.size() - disp]);
            }
        }
    }
    out.resize(size);
    return true;
}

bool is_u8(const std::vector<uint8_t>& d) {
    return d.size() >= 0x20 && be32(d.data()) == 0x55AA382D;
}

// Unpacks a U8 archive's files under `folder`.
bool u8_extract(const std::vector<uint8_t>& d, const fs::path& folder) {
    uint32_t root = be32(&d[4]);
    if (root + 12 > d.size()) {
        return false;
    }
    uint32_t total = be32(&d[root + 8]);
    size_t names = root + size_t(total) * 12;
    if (names > d.size()) {
        return false;
    }
    std::vector<std::pair<uint32_t, fs::path>> stack = {{total, folder}};  // (end index, folder)
    for (uint32_t i = 1; i < total; i++) {
        while (stack.size() > 1 && i >= stack.back().first) {
            stack.pop_back();
        }
        const uint8_t* entry = &d[root + 12 * size_t(i)];
        size_t name = names + ((entry[1] << 16) | (entry[2] << 8) | entry[3]);
        size_t end = name;
        while (end < d.size() && d[end] != 0) {
            end++;
        }
        std::string text(d.begin() + static_cast<long>(name), d.begin() + static_cast<long>(end));
        // Names stay inside the folder ("." is the folder itself, as some archives' first folder).
        bool here = text == "." && entry[0] == 1;
        if (!here && (text.empty() || text == "." || text == ".." || text.find('/') != std::string::npos || text.find('\\') != std::string::npos)) {
            return false;
        }
        fs::path path = here ? stack.back().second : stack.back().second / text;
        uint32_t a = be32(entry + 4);
        uint32_t b = be32(entry + 8);
        if (entry[0] == 1) {
            std::error_code error;
            fs::create_directories(path, error);
            stack.push_back({b, path});
        } else {
            if (size_t(a) + b > d.size() || !write_all(path, &d[a], b)) {
                return false;
            }
        }
    }
    return true;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// 32 hexadecimal digits, if that's what the text is.
bool parse_hex_key(const std::vector<uint8_t>& supplied, CommonKey& key) {
    std::string digits;
    std::string text(supplied.begin(), supplied.end());
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X') && digits.empty()) {
            i++;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) || c == '-' || c == ':') {
            continue;
        }
        if (hex_digit(c) < 0) {
            return false;
        }
        digits += c;
    }
    if (digits.size() != 32) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        key[i] = static_cast<uint8_t>(hex_digit(digits[2 * i]) * 16 + hex_digit(digits[2 * i + 1]));
    }
    return true;
}

}  // namespace

std::string key_to_hex(const CommonKey& key) {
    char text[33];
    for (int i = 0; i < 16; i++) {
        std::snprintf(text + 2 * i, 3, "%02x", key[i]);
    }
    return text;
}

bool key_from_hex(const std::string& text, CommonKey& key) {
    return parse_hex_key(std::vector<uint8_t>(text.begin(), text.end()), key);
}

KeySearch find_common_key(const std::string& wad_path, const std::vector<uint8_t>& supplied) {
    KeySearch search;
    Wad wad;
    if (!read_all(wad_path, wad.data)) {
        search.message = "The game file can't be read.";
        return search;
    }
    if (!wad.parse(search.message)) {
        return search;
    }
    const Content* test = wad.smallest();
    if (!test) {
        search.message = "This WAD file has no contents.";
        return search;
    }
    auto works = [&](const CommonKey& candidate) {
        CommonKey title = wad.title_key(candidate);
        return !wad.decrypt(*test, Aes128(title.data())).empty() || test->size == 0;
    };
    CommonKey candidate;
    if (parse_hex_key(supplied, candidate)) {
        if (works(candidate)) {
            search.found = true;
            search.key = candidate;
        } else {
            search.message = "That key doesn't decrypt this game file.";
        }
        return search;
    }
    // Binary: the key on its own, or inside a key file (e.g. a console's keys.bin) at any offset.
    if (supplied.size() >= 16 && supplied.size() <= (1u << 16)) {
        for (size_t offset = 0; offset + 16 <= supplied.size(); offset++) {
            std::memcpy(candidate.data(), &supplied[offset], 16);
            if (works(candidate)) {
                search.found = true;
                search.key = candidate;
                return search;
            }
        }
    }
    search.message = supplied.size() > (1u << 16) ? "That file is too large to be a key file."
                                                  : "No key in that file decrypts this game file.";
    return search;
}

bool installed(const fs::path& folder, const std::string& wad_sha1) {
    std::ifstream source(folder / "source.txt");
    std::string recorded;
    std::getline(source, recorded);
    std::error_code error;
    return recorded == wad_sha1 && fs::exists(folder / "main.dol", error) && fs::exists(folder / "title.tmd", error);
}

bool install_game(const std::string& wad_path, const std::string& wad_sha1, const CommonKey& key, const fs::path& folder,
                  InstallProgress& progress, std::string& error) {
    progress.fraction = 0.0f;
    Wad wad;
    if (!read_all(wad_path, wad.data)) {
        error = "The game file can't be read.";
        return false;
    }
    if (!wad.parse(error)) {
        return false;
    }
    fs::path building = folder.parent_path() / (folder.filename().string() + ".building");
    std::error_code fs_error;
    fs::remove_all(building, fs_error);
    fs::create_directories(building / "contents", fs_error);
    if (!write_all(building / "title.tmd", &wad.data[wad.tmd], wad.tmd_size)) {
        error = "Can't write to " + building.string() + ".";
        return false;
    }

    CommonKey title_key = wad.title_key(key);
    Aes128 title(title_key.data());
    bool have_program = false;
    for (size_t i = 0; i < wad.contents.size(); i++) {
        const Content& content = wad.contents[i];
        std::vector<uint8_t> data = wad.decrypt(content, title);
        if (data.empty() && content.size != 0) {
            error = "Content " + std::to_string(content.index) + " doesn't match its checksum: the key or the file is wrong.";
            fs::remove_all(building, fs_error);
            return false;
        }
        char name[16];
        std::snprintf(name, sizeof name, "%02u.app", content.index);
        if (!write_all(building / "contents" / name, data.data(), data.size())) {
            error = "Can't write the game's files.";
            fs::remove_all(building, fs_error);
            return false;
        }
        char index[8];
        std::snprintf(index, sizeof index, "%02u", content.index);
        if (is_u8(data)) {
            if (!u8_extract(data, building / "files" / index)) {
                error = "Content " + std::to_string(content.index) + " is a damaged archive.";
                fs::remove_all(building, fs_error);
                return false;
            }
        } else if (looks_lz(data)) {
            std::vector<uint8_t> program;
            if (lz_decompress(data, program) && program.size() >= 4 && be32(program.data()) == 0x100) {
                if (!write_all(building / "main.dol", program.data(), program.size())) {
                    error = "Can't write the game's files.";
                    fs::remove_all(building, fs_error);
                    return false;
                }
                have_program = true;
            }
        }
        progress.fraction = static_cast<float>(i + 1) / static_cast<float>(wad.contents.size());
    }
    if (!have_program) {
        error = "The game program wasn't found in this WAD.";
        fs::remove_all(building, fs_error);
        return false;
    }
    {
        std::ofstream source(building / "source.txt");
        source << wad_sha1 << "\n";
    }
    fs::remove_all(folder, fs_error);
    fs::rename(building, folder, fs_error);
    if (fs_error) {
        error = "Can't move the game's files into place: " + fs_error.message();
        return false;
    }
    progress.fraction = 1.0f;
    return true;
}
