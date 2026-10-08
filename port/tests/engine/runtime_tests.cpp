#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "wp/cpu.h"
#include "wp/custom_textures.h"
#include "wp/function_table.h"
#include "wp/gx_state.h"
#include "wp/input.h"
#include "wp/ios.h"
#include "wp/keymap.h"
#include "wp/modules.h"
#include "wp/options.h"
#include "wp/options_window.h"
#include "wp/save_backup.h"
#include "wp/screenshot.h"
#include "wp/settings.h"
#include "wp/ui_text.h"
#include "wp/video.h"

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "line %d: %s\n", line, expression);
        failures++;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_software_fma() {
    std::mt19937_64 random(2026);
    std::uniform_real_distribution<double> mantissa(-2.0, 2.0);
    std::uniform_int_distribution<int> exponent(-60, 60);
    int mismatches = 0;
    for (int i = 0; i < 200000; i++) {
        double a = std::ldexp(mantissa(random), exponent(random));
        double b = std::ldexp(mantissa(random), exponent(random));
        double c = std::ldexp(mantissa(random), exponent(random) * 2);
        double expected = wp::g_hardware_fma ? wp::hardware_fma(a, b, c) : std::fma(a, b, c);
        if (wp::fpr_bits(wp::software_fma(a, b, c)) != wp::fpr_bits(expected)) {
            mismatches++;
        }
    }
    CHECK(!wp::g_hardware_fma || mismatches == 0);
    CHECK(wp::fpr_bits(wp::software_fma(339099.27191141015, 292340.0665708075, -16.0)) == 0x423714bee95cb1a9ull);
}

void test_memory() {
    wp::wr32(0x80001000, 0x11223344);
    CHECK(wp::rd8(0x80001000) == 0x11);
    CHECK(wp::rd16(0x80001002) == 0x3344);
    CHECK(wp::rd32(0x80001000) == 0x11223344);
    CHECK(wp::rd32_reversed(0x80001000) == 0x44332211);
    wp::wr64(0x80001008, 0x0102030405060708ull);
    CHECK(wp::rd32(0x80001008) == 0x01020304);
    CHECK(wp::rd64(0x80001008) == 0x0102030405060708ull);
    CHECK(wp::host(0x80001000) == wp::host(0xC0001000));
    wp::wrf32(0x80001010, 1.5f);
    CHECK(wp::rd32(0x80001010) == 0x3FC00000);
    CHECK(wp::rdf32(0x80001010) == 1.5f);
    wp::wrf64(0x80001018, -2.25);
    CHECK(wp::rdf64(0x80001018) == -2.25);
    wp::wr32(0x80002000, 0xFFFFFFFF);
    wp::wr32(0x80002020, 0xFFFFFFFF);
    wp::dcbz(0x80002010);
    CHECK(wp::rd32(0x80002000) == 0);
    CHECK(wp::rd32(0x8000201C) == 0);
    CHECK(wp::rd32(0x80002020) == 0xFFFFFFFF);
}

void test_rotate_and_shift() {
    wp::Cpu c{};
    CHECK(wp::rotl(0x80000001u, 1) == 3);
    CHECK(wp::rotl(0x12345678u, 0) == 0x12345678);
    CHECK(wp::rotl(0x12345678u, 32) == 0x12345678);
    CHECK(wp::slw(1, 31) == 0x80000000);
    CHECK(wp::slw(1, 32) == 0);
    CHECK(wp::srw(0x80000000u, 31) == 1);
    CHECK(wp::srw(0x80000000u, 32) == 0);
    CHECK(wp::sraw(c, 0xFFFFFFF0u, 4) == 0xFFFFFFFF);
    CHECK(c.xer_ca == 0);
    CHECK(wp::sraw(c, 0xFFFFFFF1u, 4) == 0xFFFFFFFF);
    CHECK(c.xer_ca == 1);
    CHECK(wp::sraw(c, 0x80000000u, 40) == 0xFFFFFFFF);
    CHECK(c.xer_ca == 1);
    CHECK(wp::sraw(c, 0x40000000u, 40) == 0);
    CHECK(c.xer_ca == 0);
}

void test_arithmetic() {
    wp::Cpu c{};
    CHECK(wp::add_carry(c, 0xFFFFFFFFu, 1, 0) == 0);
    CHECK(c.xer_ca == 1);
    CHECK(wp::add_carry(c, 1, 2, 0) == 3);
    CHECK(c.xer_ca == 0);
    c.xer_ca = 1;
    CHECK(wp::add_extended(c, 1, 2) == 4);
    c.xer_ca = 0;
    CHECK(wp::sub_carry(c, 3, 5) == 2);
    CHECK(c.xer_ca == 1);
    CHECK(wp::sub_carry(c, 5, 3) == 0xFFFFFFFE);
    CHECK(c.xer_ca == 0);
    CHECK(wp::sub_carry(c, 5, 5) == 0);
    CHECK(c.xer_ca == 1);
    CHECK(wp::mulhw(0xFFFFFFFFu, 2) == 0xFFFFFFFF);
    CHECK(wp::mulhwu(0xFFFFFFFFu, 2) == 1);
    CHECK(wp::divw(static_cast<uint32_t>(-7), 2) == static_cast<uint32_t>(-3));
    CHECK(wp::divw(5, 0) == 0);
    CHECK(wp::divwu(7, 2) == 3);
    CHECK(wp::cntlzw(0) == 32);
    CHECK(wp::cntlzw(1) == 31);
    CHECK(wp::cntlzw(0x80000000u) == 0);
    CHECK(wp::sign_extend8(0x80) == 0xFFFFFF80);
    CHECK(wp::sign_extend16(0x7FFF) == 0x7FFF);
}

void test_condition_register() {
    wp::Cpu c{};
    wp::compare_signed(c, 3, 0xFFFFFFFFu, 1);
    CHECK(c.cr[3] == 8);
    wp::compare_unsigned(c, 3, 0xFFFFFFFFu, 1);
    CHECK(c.cr[3] == 4);
    wp::compare_signed(c, 3, 5, 5);
    CHECK(c.cr[3] == 2);
    CHECK(wp::cr_get(c, 14));
    CHECK(!wp::cr_get(c, 13));
    CHECK(!wp::cr_get(c, 12));
    wp::cr_set(c, 12, true);
    CHECK(c.cr[3] == 10);
    wp::mtcrf(c, 0xFF, 0x12345678);
    CHECK(wp::mfcr(c) == 0x12345678);
    wp::mtcrf(c, 0x80, 0xF0000000);
    CHECK(wp::mfcr(c) == 0xF2345678);
    c.xer_so = 1;
    wp::record(c, 0);
    CHECK(c.cr[0] == 3);
    wp::mtxer(c, 0xE0000005);
    CHECK(c.xer_so == 1 && c.xer_ov == 1 && c.xer_ca == 1 && c.xer_byte_count == 5);
    CHECK(wp::mfxer(c) == 0xE0000005);
}

void test_floating_point() {
    wp::Cpu c{};
    wp::compare_float(c, 1, 1.0, 2.0);
    CHECK(c.cr[1] == 8);
    wp::compare_float(c, 1, 2.0, 1.0);
    CHECK(c.cr[1] == 4);
    wp::compare_float(c, 1, 2.0, 2.0);
    CHECK(c.cr[1] == 2);
    wp::compare_float(c, 1, std::nan(""), 2.0);
    CHECK(c.cr[1] == 1);
    CHECK(wp::fpr_bits(wp::convert_to_int(-1.9, true)) == 0xFFF80000FFFFFFFFull);
    CHECK(wp::fpr_bits(wp::convert_to_int(2.5, false)) == 0xFFF8000000000002ull);
    CHECK(wp::fpr_bits(wp::convert_to_int(3e10, true)) == 0xFFF800007FFFFFFFull);
    CHECK(wp::fsel(-1.0, 10.0, 20.0) == 10.0);
    CHECK(wp::fsel(0.0, 10.0, 20.0) == 20.0);
    CHECK(wp::round_single(0.1) == static_cast<double>(0.1f));
}

void test_paired_singles() {
    wp::Cpu c{};
    c.f[1] = 1.0;
    c.ps1[1] = 2.0;
    c.f[2] = 10.0;
    c.ps1[2] = 20.0;
    wp::ps_add(c, 3, 1, 2);
    CHECK(c.f[3] == 11.0 && c.ps1[3] == 22.0);
    wp::ps_merge(c, 4, 1, 2, true, false);
    CHECK(c.f[4] == 2.0 && c.ps1[4] == 10.0);
    wp::ps_madd(c, 5, 1, 2, 1, 1.0, 1.0);
    CHECK(c.f[5] == 11.0 && c.ps1[5] == 42.0);
    wp::ps_madd(c, 5, 1, 2, 1, -1.0, 1.0);
    CHECK(c.f[5] == -9.0 && c.ps1[5] == -38.0);
    wp::ps_sum0(c, 6, 1, 2, 1);
    CHECK(c.f[6] == 3.0 && c.ps1[6] == 20.0);
    wp::ps_muls1(c, 7, 1, 2);
    CHECK(c.f[7] == 20.0 && c.ps1[7] == 40.0);
}

void test_quantization() {
    wp::Cpu c{};
    uint32_t size = 0;
    c.spr[wp::kSprGqr0] = 0;
    wp::wrf32(0x80003000, 3.5f);
    wp::wrf32(0x80003004, -1.25f);
    wp::load_quantized(c, 1, 0x80003000, 0, 0, size);
    CHECK(c.f[1] == 3.5 && c.ps1[1] == -1.25 && size == 8);
    wp::load_quantized(c, 2, 0x80003000, 1, 0, size);
    CHECK(c.f[2] == 3.5 && c.ps1[2] == 1.0 && size == 4);

    c.spr[wp::kSprGqr0 + 1] = (7u << 16) | (4u << 24);
    wp::wr16(0x80003010, 0xFFF0);
    wp::wr16(0x80003012, 32);
    wp::load_quantized(c, 3, 0x80003010, 0, 1, size);
    CHECK(c.f[3] == -1.0 && c.ps1[3] == 2.0 && size == 4);

    c.spr[wp::kSprGqr0 + 2] = 7u | (4u << 8);
    c.f[4] = -1.0;
    c.ps1[4] = 2.0;
    wp::store_quantized(c, 4, 0x80003020, 0, 2, size);
    CHECK(wp::rd16(0x80003020) == 0xFFF0 && wp::rd16(0x80003022) == 32 && size == 4);

    c.spr[wp::kSprGqr0 + 3] = 4u | (0u << 8);
    c.f[5] = 300.0;
    wp::store_quantized(c, 5, 0x80003030, 1, 3, size);
    CHECK(wp::rd8(0x80003030) == 255 && size == 1);
}

void test_disc_drive() {
    int32_t drive = wp::ios::open("/dev/di");
    const uint32_t command = 0x80200000;
    const uint32_t output = 0x80201000;

    wp::wr32(command + 4, 0x20);
    wp::wr32(command + 8, 0x460A0000);
    CHECK(wp::ios::ioctl(drive, 0x8D, command, 0x20, output, 0x20) == 2);
    CHECK(wp::ios::ioctl(drive, 0xE0, command, 0x20, output, 0x20) == 1);
    CHECK(wp::rd32(output) == 0x52100);
    CHECK(wp::ios::ioctl(drive, 0xE0, command, 0x20, output, 0x20) == 1);
    CHECK(wp::rd32(output) == 0);

    wp::wr32(command + 8, 0);
    CHECK(wp::ios::ioctl(drive, 0x8D, command, 0x20, output, 0x20) == 1);

    CHECK(wp::ios::ioctl(drive, 0xA4, command, 0x20, output, 0x20) == 2);
    CHECK(wp::ios::ioctl(drive, 0xE0, command, 0x20, output, 0x20) == 1);
    CHECK(wp::rd32(output) == 0x53100);
}

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void test_save_backups() {
    namespace fs = std::filesystem;
    using wp::saves::Result;
    std::error_code error;
    fs::path root = fs::temp_directory_path(error) / "wp_save_backup_tests";
    fs::remove_all(root, error);
    fs::path nand = root / "nand";
    fs::path backups = wp::saves::backups_beside(nand);
    fs::path relative = wp::saves::save_relative(0x53555045);
    fs::path save = nand / relative;
    const std::time_t start = 1790000000;
    CHECK(relative == fs::path("title/00010000/53555045/data"));
    CHECK(backups == root / "backups");
    CHECK(wp::saves::backups_beside(fs::path(nand) += "/") == backups);

    CHECK(wp::saves::back_up(nand, relative, backups, 5, start).result == Result::NoSave);
    CHECK(wp::saves::list(backups).empty());

    write_file(save / "wiiparty.bin", "first");
    write_file(save / "banner.bin", "banner");
    std::string problem;
    CHECK(wp::saves::validate(save, problem));
    wp::saves::Outcome first = wp::saves::back_up(nand, relative, backups, 5, start);
    CHECK(first.result == Result::Created);
    CHECK(wp::saves::list(backups).size() == 1);
    CHECK(wp::saves::same_contents(save, first.backup / relative));
    CHECK(read_file(first.backup / relative / "wiiparty.bin") == "first");

    CHECK(wp::saves::back_up(nand, relative, backups, 5, start + 10).result == Result::Unchanged);
    CHECK(wp::saves::list(backups).size() == 1);

    write_file(save / "wiiparty.bin", "second");
    wp::saves::Outcome same_second = wp::saves::back_up(nand, relative, backups, 5, start);
    CHECK(same_second.result == Result::Created);
    std::vector<wp::saves::Backup> listed = wp::saves::list(backups);
    CHECK(listed.size() == 2 && listed.front().path == same_second.backup && listed.back().path == first.backup);

    for (int i = 0; i < 4; i++) {
        write_file(save / "wiiparty.bin", "round " + std::to_string(i));
        CHECK(wp::saves::back_up(nand, relative, backups, 3, start + 100 + i).result == Result::Created);
    }
    listed = wp::saves::list(backups);
    CHECK(listed.size() == 3);
    CHECK(listed.size() == 3 && read_file(listed[0].path / relative / "wiiparty.bin") == "round 3");
    CHECK(listed.size() == 3 && read_file(listed[2].path / relative / "wiiparty.bin") == "round 1");
    CHECK(!fs::exists(first.backup) && !fs::exists(same_second.backup));
    CHECK(read_file(save / "wiiparty.bin") == "round 3");

    write_file(save / "wiiparty.bin", "current");
    wp::saves::Backup oldest = listed.back();
    CHECK(wp::saves::saved_relative(oldest) == relative);
    wp::saves::Outcome restored = wp::saves::restore(nand, backups, oldest, 3, start + 200);
    CHECK(restored.result == Result::Created);
    CHECK(read_file(save / "wiiparty.bin") == "round 1");
    CHECK(read_file(save / "banner.bin") == "banner");
    listed = wp::saves::list(backups);
    CHECK(listed.size() == 3 && read_file(listed[0].path / relative / "wiiparty.bin") == "current");
    CHECK(!fs::exists(fs::path(save) += ".replaced") && !fs::exists(fs::path(save) += ".restoring"));
    CHECK(wp::saves::restore(nand, backups, listed[0], 3, start + 300).result == Result::Created);
    CHECK(read_file(save / "wiiparty.bin") == "current");
    CHECK(wp::saves::restore(nand, backups, listed[0], 3, start + 400).result == Result::Unchanged);
    listed = wp::saves::list(backups);
    CHECK(listed.size() == 3 && read_file(listed[0].path / relative / "wiiparty.bin") == "round 1");

    wp::saves::Backup empty{"2026-01-01_00-00-00", backups / "2026-01-01_00-00-00"};
    fs::create_directories(empty.path);
    CHECK(wp::saves::restore(nand, backups, empty, 3, start + 500).result == Result::Failed);
    CHECK(read_file(save / "wiiparty.bin") == "current");
    fs::create_directories(backups / "notes");
    fs::create_directories(backups / "2026-01-01_00-00-00.partial");
    CHECK(wp::saves::list(backups).size() == 4);
    fs::remove_all(root, error);
}

void test_custom_texture_hash() {
    using wp::gx::custom_textures::xxh64;
    const char* sentence = "Nobody inspects the spammish repetition";
    uint8_t sequence[256];
    for (int i = 0; i < 256; i++) {
        sequence[i] = static_cast<uint8_t>(i);
    }
    CHECK(xxh64("", 0) == 0xEF46DB3751D8E999ull);
    CHECK(xxh64("a", 1) == 0xD24EC4F1A98C6E5Bull);
    CHECK(xxh64("abc", 3) == 0x44BC2CF5AD770999ull);
    CHECK(xxh64(sentence, std::strlen(sentence)) == 0xFBCEA83C8A378BF1ull);
    CHECK(xxh64(sequence, sizeof sequence) == 0x1FACBE8406CD904Bull);
    CHECK(xxh64("", 0, 0x9E3779B1) == 0xAC75FDA2929B17EFull);
    CHECK(xxh64("abc", 3, 0x9E3779B1) == 0x1318DF30094A85FDull);
    CHECK(xxh64(sentence, std::strlen(sentence), 0x9E3779B1) == 0x56DB22DD5B051147ull);
    CHECK(xxh64(sequence, sizeof sequence, 0x9E3779B1) == 0xD48195F45908996Cull);
}

void test_custom_texture_names() {
    namespace ct = wp::gx::custom_textures;
    const uint8_t c4[] = {0x35, 0x74};
    ct::PaletteRange range = ct::used_palette_range(c4, sizeof c4, 8);
    CHECK(range.first == 3 && range.count == 5);
    const uint8_t c8[] = {10, 200, 50};
    range = ct::used_palette_range(c8, sizeof c8, 9);
    CHECK(range.first == 10 && range.count == 191);
    const uint8_t c14[] = {0x12, 0x34, 0x00, 0x05, 0xC0, 0x07};
    range = ct::used_palette_range(c14, sizeof c14, 10);
    CHECK(range.first == 5 && range.count == 0x1230);
    range = ct::used_palette_range(c8, sizeof c8, 4);
    CHECK(range.count == 0);

    uint8_t data[32];
    for (int i = 0; i < 32; i++) {
        data[i] = static_cast<uint8_t>(i);
    }
    ct::TextureName plain = ct::name_texture(data, sizeof data, 4, 4, 4, false, nullptr);
    CHECK(plain.full() == "tex1_4x4_cbf59c5116ff32b4_4");
    CHECK(plain.wildcard() == "tex1_4x4_cbf59c5116ff32b4_$_4");
    CHECK(ct::name_texture(data, sizeof data, 4, 4, 4, true, nullptr).full() == "tex1_4x4_m_cbf59c5116ff32b4_4");

    uint8_t indices[32];
    for (int i = 0; i < 32; i++) {
        indices[i] = static_cast<uint8_t>(i % 4 + 2);
    }
    uint8_t tlut[512];
    for (int i = 0; i < 512; i++) {
        tlut[i] = static_cast<uint8_t>(i);
    }
    ct::TextureName palette = ct::name_texture(indices, sizeof indices, 8, 4, 9, false, tlut);
    CHECK(palette.full() == "tex1_8x4_5e9da7e61227046a_6ac8c5a2eb076b8a_9");
    CHECK(palette.wildcard() == "tex1_8x4_5e9da7e61227046a_$_9");
    CHECK(ct::level_name(palette.full(), 0) == palette.full());
    CHECK(ct::level_name(palette.full(), 2) == palette.full() + "_mip2");
}

void test_custom_texture_index() {
    namespace ct = wp::gx::custom_textures;
    ct::Index index;
    ct::TextureName name{"tex1_8x4", "5e9da7e61227046a", "_6ac8c5a2eb076b8a", "9"};
    CHECK(index.resolve(name).empty());
    CHECK(index.add("pack/menus/TEX1_8x4_5E9DA7E61227046A_6AC8C5A2EB076B8A_9.PNG"));
    CHECK(!index.add("pack/tex1_8x4_5e9da7e61227046a_6ac8c5a2eb076b8a_9.dds"));
    CHECK(!index.add("pack/readme.png"));
    CHECK(index.add("pack/tex1_4x4_0000000000000001_5_arb.png"));
    CHECK(index.add("pack/tex1_4x4_0000000000000001_5_mip1.png"));
    CHECK(index.size() == 3);
    CHECK(index.resolve(name) == "tex1_8x4_5e9da7e61227046a_6ac8c5a2eb076b8a_9");
    CHECK(index.find("tex1_4x4_0000000000000001_5") != nullptr);
    CHECK(index.find(ct::level_name("tex1_4x4_0000000000000001_5", 1)) != nullptr);
    CHECK(index.find(ct::level_name("tex1_4x4_0000000000000001_5", 2)) == nullptr);
    CHECK(index.add("pack/tex1_8x4_5e9da7e61227046a_$_9.png"));
    CHECK(index.resolve(name) == "tex1_8x4_5e9da7e61227046a_$_9");
    name.texture = "0000000000000000";
    CHECK(index.resolve(name).empty());

    std::error_code error;
    std::filesystem::path root = std::filesystem::temp_directory_path(error) / "wp_custom_textures_test";
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "SUPP01" / "menus", error);
    std::ofstream(root / "SUPP01" / "menus" / "tex1_4x4_cbf59c5116ff32b4_4.png") << "x";
    std::ofstream(root / "SUPP01" / "notes.txt") << "x";
    ct::Index scanned;
    CHECK(scanned.scan(root) == 1);
    CHECK(scanned.find("tex1_4x4_cbf59c5116ff32b4_4") != nullptr);
    CHECK(scanned.scan(root / "missing") == 0);

    std::vector<uint32_t> pixels = {0xFF0000FFu, 0x8000FF00u, 0x00FF0000u, 0x12345678u, 0xFFFFFFFFu, 0x00000000u};
    std::filesystem::path png = root / "round_trip.png";
    wp::video::save_png_rgba(png.string().c_str(), pixels, 3, 2);
    std::vector<uint32_t> decoded;
    uint32_t width = 0;
    uint32_t height = 0;
    CHECK(ct::decode_png(png, decoded, width, height));
    CHECK(width == 3 && height == 2 && decoded == pixels);
    CHECK(!ct::decode_png(root / "SUPP01" / "menus" / "tex1_4x4_cbf59c5116ff32b4_4.png", decoded, width, height));
    CHECK(width == 3 && height == 2 && decoded == pixels);
    std::filesystem::remove_all(root, error);
}

int blend_factor(wp::gx::BlendFactor factor, int written, int tev, int destination) {
    using wp::gx::BlendFactor;
    switch (factor) {
    case BlendFactor::Zero:
        return 0;
    case BlendFactor::One:
        return 255;
    case BlendFactor::SourceColor:
    case BlendFactor::SourceAlpha:
        return written;
    case BlendFactor::InverseSourceColor:
    case BlendFactor::InverseSourceAlpha:
        return 255 - written;
    case BlendFactor::DestinationColor:
    case BlendFactor::DestinationAlpha:
        return destination;
    case BlendFactor::InverseDestinationColor:
    case BlendFactor::InverseDestinationAlpha:
        return 255 - destination;
    case BlendFactor::TevColor:
    case BlendFactor::TevAlpha:
        return tev;
    case BlendFactor::InverseTevAlpha:
        return 255 - tev;
    }
    return -1;
}

int blend_channel(const wp::gx::BlendState& state, bool alpha, int tev, int destination) {
    using wp::gx::LogicSource;
    int written = state.output == LogicSource::Tev ? tev : (state.output == LogicSource::Zero ? 0 : (state.output == LogicSource::One ? 255 : 255 - tev));
    if (!state.enable) {
        return written;
    }
    double source = written * blend_factor(alpha ? state.source_alpha : state.source, written, tev, destination) / (255.0 * 255.0);
    double target = destination * blend_factor(alpha ? state.destination_alpha : state.destination, written, tev, destination) / (255.0 * 255.0);
    wp::gx::BlendOperation operation = alpha ? state.alpha_operation : state.operation;
    double value = operation == wp::gx::BlendOperation::Add ? source + target
                   : (operation == wp::gx::BlendOperation::Subtract ? source - target : target - source);
    value = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
    return static_cast<int>(value * 255.0 + 0.5);
}

int logic_reference(uint32_t op, int s, int d) {
    int results[16] = {0, s & d, s & ~d, s, ~s & d, d, s ^ d, s | d, ~(s | d), ~(s ^ d), ~d, s | ~d, ~s, ~s | d, ~(s & d), 255};
    return results[op] & 255;
}

void test_gx_logic_ops() {
    using namespace wp::gx;
    for (uint32_t op = 0; op < 16; op++) {
        uint32_t mode = 2 | (1u << 3) | (1u << 4) | (op << 12);
        BlendState state = blend_state(mode, true, false, true);
        CHECK(state.logic_op);
        CHECK(state.logic_mode == op);
        if (op == 5) {
            CHECK(!state.color_update && !state.alpha_update && !state.enable);
            CHECK(blend_state(mode, true, true, true).alpha_update);
            continue;
        }
        CHECK(state.color_update && state.alpha_update);
        bool binary = true;
        for (int s = 0; s <= 255; s += 255) {
            for (int d = 0; d <= 255; d += 255) {
                binary &= blend_channel(state, false, s, d) == logic_reference(op, s, d);
                binary &= blend_channel(state, true, s, d) == logic_reference(op, s, d);
            }
        }
        CHECK(binary);
        bool exact = true;
        for (int s = 0; s < 256; s++) {
            for (int d = 0; d < 256; d++) {
                exact &= blend_channel(state, false, s, d) == logic_reference(op, s, d) && blend_channel(state, true, s, d) == logic_reference(op, s, d);
            }
        }
        CHECK(exact == logic_op_exact(op));
        BlendState constant = blend_state(mode, true, true, true);
        CHECK(!constant.enable || (constant.source_alpha == BlendFactor::One && constant.destination_alpha == BlendFactor::Zero &&
                                   constant.alpha_operation == BlendOperation::Add));
    }
    CHECK(logic_op_exact(0) && logic_op_exact(3) && logic_op_exact(5) && logic_op_exact(10) && logic_op_exact(12) && logic_op_exact(15));
    CHECK(!logic_op_exact(1) && !logic_op_exact(6) && !logic_op_exact(7) && !logic_op_exact(14));
    CHECK(std::string(logic_op_name(6)) == "xor");

    uint32_t all = 1 | 2 | (1u << 11) | (1u << 3) | (1u << 4) | (6u << 12);
    BlendState subtract = blend_state(all, true, false, true);
    CHECK(subtract.enable && !subtract.logic_op && subtract.operation == BlendOperation::ReverseSubtract);
    CHECK(subtract.source == BlendFactor::One && subtract.destination == BlendFactor::One);
    BlendState subtract_alone = blend_state((1u << 11) | (1u << 3), true, false, true);
    CHECK(!subtract_alone.enable && !subtract_alone.logic_op);
    BlendState subtract_logic = blend_state(2 | (1u << 11) | (1u << 3) | (6u << 12), true, false, true);
    CHECK(subtract_logic.logic_op && subtract_logic.logic_mode == 6 && subtract_logic.operation == BlendOperation::Add);
    BlendState subtract_constant = blend_state(all, true, true, true);
    CHECK(subtract_constant.alpha_operation == BlendOperation::Add && subtract_constant.destination_alpha == BlendFactor::Zero);

    uint32_t blend = 1 | 2 | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5) | (6u << 12);
    BlendState alpha_blend = blend_state(blend, true, false, true);
    CHECK(alpha_blend.enable && !alpha_blend.logic_op && alpha_blend.operation == BlendOperation::Add);
    CHECK(alpha_blend.source == BlendFactor::TevAlpha && alpha_blend.destination == BlendFactor::InverseTevAlpha);
    CHECK(alpha_blend.source_alpha == BlendFactor::TevAlpha && alpha_blend.destination_alpha == BlendFactor::InverseTevAlpha);
    CHECK(alpha_blend.output == LogicSource::Tev);
    BlendState color_blend = blend_state(1 | (1u << 3) | (1u << 4) | (2u << 8) | (3u << 5), true, false, true);
    CHECK(color_blend.source == BlendFactor::DestinationColor && color_blend.destination == BlendFactor::InverseSourceColor);
    CHECK(color_blend.source_alpha == BlendFactor::DestinationAlpha && color_blend.destination_alpha == BlendFactor::InverseTevAlpha);
    BlendState no_alpha = blend_state(1 | (1u << 3) | (1u << 4) | (6u << 8) | (7u << 5), false, false, true);
    CHECK(no_alpha.source == BlendFactor::One && no_alpha.destination == BlendFactor::Zero && !no_alpha.alpha_update);
    BlendState constant_blend = blend_state(blend, true, true, true);
    CHECK(constant_blend.source_alpha == BlendFactor::One && constant_blend.destination_alpha == BlendFactor::Zero);
    BlendState failing = blend_state(blend, true, false, false);
    CHECK(!failing.color_update && !failing.alpha_update);
    BlendState disabled = blend_state((1u << 3) | (1u << 4) | (4u << 8), true, false, true);
    CHECK(!disabled.enable && !disabled.logic_op && disabled.output == LogicSource::Tev);
}

void test_gx_line_point_offsets() {
    using namespace wp::gx;
    uint32_t bp[256] = {};
    CHECK(line_point_offsets(bp).line_coordinates == 0 && line_point_offsets(bp).point_coordinates == 0);
    bp[0x30] = (1u << 18) | 0xFF;
    bp[0x32] = 1u << 19;
    bp[0x34] = 3u << 18;
    bp[0x31] = 3u << 18;
    CHECK(line_point_offsets(bp).line_coordinates == 0 && line_point_offsets(bp).point_coordinates == 0);
    bp[0x22] = (3u << 16) | (5u << 19) | 0x1234;
    LinePointOffsets offsets = line_point_offsets(bp);
    CHECK(offsets.line == 0.25f && offsets.point == 1.0f);
    CHECK(offsets.line_coordinates == 5 && offsets.point_coordinates == 6);
    const float expected[8] = {0.0f, 1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f, 1.0f, 1.0f, 1.0f};
    for (uint32_t code = 0; code < 8; code++) {
        bp[0x22] = (code << 16) | (code << 19);
        CHECK(line_point_offsets(bp).line == expected[code] && line_point_offsets(bp).point == expected[code]);
    }
    CHECK(line_offset_negative_side(0.5f, 0.1f, false));
    CHECK(!line_offset_negative_side(-0.5f, 0.1f, false));
    CHECK(line_offset_negative_side(0.1f, -0.5f, true));
    CHECK(!line_offset_negative_side(0.1f, 0.5f, true));
    ScreenVertex vertex{};
    vertex.uv[0][0] = 0.5f;
    vertex.uv[2][1] = 0.25f;
    vertex.uv[0][2] = 1.0f;
    vertex.uv[2][2] = 1.0f;
    offset_texture_coordinates(vertex, 5, 0.125f, 0.5f);
    CHECK(vertex.uv[0][0] == 0.625f && vertex.uv[0][1] == 0.5f);
    CHECK(vertex.uv[1][0] == 0.0f && vertex.uv[1][1] == 0.0f);
    CHECK(vertex.uv[2][0] == 0.125f && vertex.uv[2][1] == 0.75f);
}

void test_key_names() {
    using namespace wp::keymap;
    CHECK(key_code("A") == 'A');
    CHECK(key_code("a") == 'A');
    CHECK(key_code("7") == '7');
    CHECK(key_code("F1") == 0x70);
    CHECK(key_code("f24") == 0x87);
    CHECK(key_code("F25") == -1);
    CHECK(key_code("F0") == -1);
    CHECK(key_code("F01") == -1);
    CHECK(key_code("Enter") == 0x0D);
    CHECK(key_code(" space ") == 0x20);
    CHECK(key_code("BACKSPACE") == 0x08);
    CHECK(key_code("Tab") == 0x09);
    CHECK(key_code("Shift") == 0x10);
    CHECK(key_code("LeftShift") == 0xA0);
    CHECK(key_code("Ctrl") == 0x11);
    CHECK(key_code("Alt") == 0x12);
    CHECK(key_code("Up") == 0x26);
    CHECK(key_code("Down") == 0x28);
    CHECK(key_code("Left") == 0x25);
    CHECK(key_code("Right") == 0x27);
    CHECK(key_code("Plus") == 0xBB);
    CHECK(key_code("Minus") == 0xBD);
    CHECK(key_code("NumPlus") == 0x6B);
    CHECK(key_code("MouseLeft") == 0x01);
    CHECK(key_code("MouseRight") == 0x02);
    CHECK(key_code("MouseMiddle") == 0x04);
    CHECK(key_code("MouseX1") == 0x05);
    CHECK(key_code("MouseX2") == 0x06);
    CHECK(key_code("WheelUp") == kWheelUp);
    CHECK(key_code("WheelDown") == kWheelDown);
    CHECK(key_code("") == -1);
    CHECK(key_code("Banana") == -1);
    CHECK(key_code("AB") == -1);
    CHECK(key_name('Q') == "Q");
    CHECK(key_name(0x7A) == "F11");
    CHECK(key_name(0xA0) == "LeftShift");
    CHECK(key_name(kWheelDown) == "WheelDown");
    CHECK(key_name(0x3000).empty());
    for (int code = 0; code < 0x200; code++) {
        std::string name = key_name(code);
        CHECK(name.empty() || key_code(name) == code);
    }
    CHECK(reserved(key_code("F11")));
    CHECK(reserved(key_code("F12")));
    CHECK(reserved(key_code("F10")));
    CHECK(!reserved(key_code("F9")));
    Parsed parsed = parse(" Enter , space,MouseLeft,,Nope,F12,enter,F11 ");
    CHECK(parsed.codes.size() == 3);
    CHECK(format(parsed.codes) == "Enter,Space,MouseLeft");
    CHECK(format(parsed.codes, ", ") == "Enter, Space, MouseLeft");
    CHECK(parsed.invalid.size() == 3);
    CHECK(parsed.invalid.size() == 3 && parsed.invalid[0] == "Nope" && parsed.invalid[1] == "F12" && parsed.invalid[2] == "F11");
    CHECK(parse("").codes.empty() && parse("").invalid.empty());
}

void test_key_defaults() {
    using namespace wp::keymap;
    Bindings keys = defaults();
    CHECK(format(keys.of(Action::A)) == "Enter,Space,MouseLeft");
    CHECK(format(keys.of(Action::B)) == "Backspace,MouseRight");
    CHECK(format(keys.of(Action::Plus)) == "Plus,NumPlus");
    CHECK(format(keys.of(Action::Minus)) == "Minus,NumMinus");
    CHECK(format(keys.of(Action::Home)) == "H");
    CHECK(format(keys.of(Action::Up)) == "Up,A");
    CHECK(format(keys.of(Action::Right)) == "Right,W");
    CHECK(format(keys.of(Action::Shake)) == "MouseMiddle,LeftShift");
    CHECK(format(keys.of(Action::SwingUp)) == "WheelUp,T");
    CHECK(format(keys.of(Action::SwingDown)) == "WheelDown,G");
    CHECK(format(keys.of(Action::Grip)) == "Tab");
    CHECK(format(keys.of(Action::Screenshot)) == "F9");
    CHECK(action(Action::Screenshot).buttons == 0);
    CHECK(setting_key(static_cast<size_t>(Action::Screenshot)) == "keys.screenshot");
    CHECK(action(Action::A).buttons == wp::input::kButtonA);
    CHECK(action(Action::TiltDown).buttons == wp::input::kMotionTiltDown);
    CHECK(action(Action::Grip).buttons == 0);
    CHECK(keys.has(Action::A, 0x0D));
    CHECK(!keys.has(Action::B, 0x0D));
    CHECK(keys.bound('Q'));
    CHECK(!keys.bound(0x7A));
    CHECK(!keys.bound(0x7B));
    for (size_t i = 0; i < kActionCount; i++) {
        CHECK(parse(action(i).defaults).invalid.empty());
        CHECK(!keys.codes[i].empty());
        CHECK(setting_key(i) == std::string("keys.") + action(i).name);
    }
    std::map<std::string, std::string> file = {{"keys.a", "K,Wrong"}, {"keys.grip", ""}, {"keys.b", "F12"}};
    std::vector<std::string> invalid;
    Bindings loaded = load(
        [&](const std::string& key) {
            auto it = file.find(key);
            if (it != file.end()) {
                return it->second;
            }
            for (size_t i = 0; i < kActionCount; i++) {
                if (setting_key(i) == key) {
                    return std::string(action(i).defaults);
                }
            }
            return std::string();
        },
        invalid);
    CHECK(format(loaded.of(Action::A)) == "K");
    CHECK(loaded.of(Action::Grip).empty());
    CHECK(loaded.of(Action::B).empty());
    CHECK(format(loaded.of(Action::Home)) == "H");
    CHECK(invalid.size() == 2 && invalid[0] == "keys.a: Wrong" && invalid[1] == "keys.b: F12");
}

void test_settings_file_keys() {
    const char* path = "runtime_tests_settings.ini";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "[keys]\na = K, MouseX1\nunknown=1\n";
    }
    wp::settings::load(path);
    CHECK(wp::settings::text("keys.a", nullptr) == "K, MouseX1");
    CHECK(wp::settings::text("keys.grip", nullptr) == "Tab");
    CHECK(wp::settings::text("video.scale", nullptr) == "1");
    std::ifstream file(path, std::ios::binary);
    std::stringstream contents;
    contents << file.rdbuf();
    std::string text = contents.str();
    CHECK(text.find("[keys]\n") != std::string::npos);
    CHECK(text.find("a=K, MouseX1\n") != std::string::npos);
    CHECK(text.find("unknown") == std::string::npos);
    CHECK(text.find("[video]\nscale=1\n") == 0);
    for (size_t i = 0; i < wp::keymap::kActionCount; i++) {
        std::string line = "\n" + std::string(wp::keymap::action(i).name) + "=";
        CHECK(text.find(line, text.find("[keys]")) != std::string::npos);
    }
    file.close();
    std::remove(path);
}

void test_screenshot_names() {
    std::tm time{};
    time.tm_year = 2026 - 1900;
    time.tm_mon = 8;
    time.tm_mday = 6;
    time.tm_hour = 9;
    time.tm_min = 5;
    time.tm_sec = 3;
    CHECK(wp::screenshot::file_name("wiiparty", time, 1) == "wiiparty_2026-09-06_09-05-03.png");
    CHECK(wp::screenshot::file_name("wiiparty", time, 2) == "wiiparty_2026-09-06_09-05-03_2.png");
    CHECK(wp::screenshot::file_name("wiiparty", time, 13) == "wiiparty_2026-09-06_09-05-03_13.png");
    time.tm_mon = 11;
    time.tm_mday = 31;
    time.tm_hour = 23;
    time.tm_min = 59;
    time.tm_sec = 59;
    CHECK(wp::screenshot::file_name("game", time, 1) == "game_2026-12-31_23-59-59.png");
    CHECK(std::string(wp::screenshot::kFolder) == "screenshots");
    std::vector<uint8_t> png = wp::video::encode_png({0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF, 0, 0x00123456}, 3, 2);
    const uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(png.size() > 45 && std::equal(signature, signature + 8, png.begin()));
    CHECK(png.size() > 24 && png[12] == 'I' && png[13] == 'H' && png[14] == 'D' && png[15] == 'R' && png[19] == 3 && png[23] == 2);
}

void test_live_settings() {
    const char* path = "runtime_tests_settings.ini";
    wp::settings::load(path);
    wp::settings::LiveFlag mute("audio.mute", nullptr);
    wp::settings::LiveNumber scale("video.scale", nullptr);
    CHECK(!mute());
    CHECK(scale() == 1);
    uint32_t revision = wp::settings::revision();
    wp::settings::store("audio.mute", "1");
    wp::settings::store("video.scale", "3");
    CHECK(wp::settings::revision() == revision + 2);
    CHECK(mute());
    CHECK(scale() == 3);
    wp::settings::load(path);
    CHECK(mute());
    CHECK(scale() == 3);
    std::vector<std::string> keys = wp::settings::keys();
    CHECK(std::find(keys.begin(), keys.end(), "system.options_menu") != keys.end());
    CHECK(wp::settings::flag("system.options_menu", nullptr));
    wp::settings::load("");
    CHECK(!mute());
    CHECK(scale() == 1);
    std::remove(path);
}

void test_options_menu() {
    wp::settings::load("");
    std::vector<std::string> keys = wp::settings::keys();
    for (wp::ui::Language language : {wp::ui::Language::English, wp::ui::Language::Spanish}) {
        wp::ui::set_language(language);
        for (const std::string& key : keys) {
            CHECK(key != wp::ui::label(key.c_str()));
            CHECK(!wp::options::choices(key).empty());
        }
    }
    wp::ui::set_language(wp::ui::Language::English);
    CHECK(wp::options::live("video.scale"));
    CHECK(wp::options::live("video.fullscreen"));
    CHECK(wp::options::live("audio.mute"));
    CHECK(!wp::options::live("system.language"));
    CHECK(!wp::options::live("system.pal60"));
    CHECK(wp::options::step_value("audio.mute", "0", 1) == "1");
    CHECK(wp::options::step_value("audio.mute", "1", 1) == "0");
    CHECK(wp::options::step_value("audio.mute", "off", -1) == "1");
    CHECK(wp::options::step_value("audio.mute", "yes", 1) == "0");
    CHECK(wp::options::step_value("video.scale", "6", 1) == "1");
    CHECK(wp::options::step_value("video.scale", "1", -1) == "6");
    CHECK(wp::options::step_value("saves.backups", "5", 1) == "10");
    CHECK(wp::options::step_value("saves.backups", "0", -1) == "20");
    CHECK(wp::options::step_value("saves.backups", "7", 1) == "10");
    CHECK(wp::options::shown_value("saves.backups", "0") == "Off");
    CHECK(wp::options::live("saves.backups"));
    CHECK(!wp::options::live("video.custom_textures"));
    CHECK(std::find(keys.begin(), keys.end(), "keys.a") == keys.end());
    CHECK(wp::options::step_value("video.scale", "9", 1) == "1");
    CHECK(wp::options::step_value("system.language", "auto", 1) == "en");
    CHECK(wp::options::step_value("system.language", "auto", -1) == "nl");
    CHECK(wp::options::step_value("unknown.key", "x", 1) == "x");
    CHECK(wp::options::shown_value("video.scale", "1") == "Native");
    CHECK(wp::options::shown_value("video.scale", "4") == "4x");
    CHECK(wp::options::shown_value("system.language", "auto") == "System");
    CHECK(wp::options::shown_value("system.language", "de") == "Deutsch");
    CHECK(wp::options::shown_value("audio.mute", "1") == "On");
    wp::ui::set_language(wp::ui::Language::Spanish);
    CHECK(wp::options::shown_value("audio.mute", "0") == "No");
    CHECK(std::strcmp(wp::ui::label("audio.mute"), "Silenciar") == 0);
    wp::ui::set_language(wp::ui::Language::English);

    wp::options::Menu menu;
    CHECK(!menu.open());
    menu.set_open(true);
    std::vector<wp::options::Row> rows = menu.rows();
    CHECK(rows.size() == keys.size());
    CHECK(rows[0].key == "video.scale" && rows[0].value == "Native" && rows[0].live);
    CHECK(menu.selection() == 0);
    CHECK(menu.handle(wp::options::Action::Up).empty());
    CHECK(menu.selection() == keys.size() - 1);
    CHECK(menu.handle(wp::options::Action::Down).empty());
    CHECK(menu.selection() == 0);
    CHECK(menu.handle(wp::options::Action::Next) == "video.scale");
    CHECK(wp::settings::number("video.scale", nullptr) == 2);
    CHECK(menu.rows()[0].value == "2x");
    CHECK(menu.handle(wp::options::Action::Previous) == "video.scale");
    CHECK(menu.handle(wp::options::Action::Previous) == "video.scale");
    CHECK(wp::settings::number("video.scale", nullptr) == 6);
    size_t mute = static_cast<size_t>(std::find(keys.begin(), keys.end(), "audio.mute") - keys.begin());
    menu.select(mute);
    CHECK(menu.selection() == mute);
    menu.select(keys.size());
    CHECK(menu.selection() == mute);
    wp::settings::LiveFlag muted("audio.mute", nullptr);
    CHECK(!muted());
    CHECK(menu.handle(wp::options::Action::Next) == "audio.mute");
    CHECK(muted());
    CHECK(menu.handle(wp::options::Action::Close).empty());
    CHECK(!menu.open());
    wp::settings::load("");
}

void test_options_repeat_and_layout() {
    wp::options::Repeat repeat(0x3, 400, 100);
    CHECK(repeat.update(0x1, 0) == 0x1);
    CHECK(repeat.update(0x1, 100) == 0);
    CHECK(repeat.update(0x1, 399) == 0);
    CHECK(repeat.update(0x1, 400) == 0x1);
    CHECK(repeat.update(0x1, 450) == 0);
    CHECK(repeat.update(0x1, 500) == 0x1);
    CHECK(repeat.update(0x5, 510) == 0x4);
    CHECK(repeat.update(0x5, 700) == 0x1);
    CHECK(repeat.update(0x0, 800) == 0);
    CHECK(repeat.update(0x4, 810) == 0x4);
    CHECK(repeat.update(0x4, 5000) == 0);

    wp::options::Layout frame = wp::options::layout(720, 12);
    CHECK(frame.unit == 18);
    CHECK(frame.height == frame.padding * 2 + frame.title_height + 12 * frame.row_height + frame.footer_height);
    CHECK(frame.height <= 720);
    CHECK(wp::options::layout(100, 12).unit == 14);
    CHECK(wp::options::layout(4000, 12).unit == 40);
    wp::options::Rect placed = wp::options::place(1280, 720, frame.width, frame.height);
    CHECK(placed.width == frame.width && placed.height == frame.height);
    CHECK(placed.x == (1280 - frame.width) / 2 && placed.y == (720 - frame.height) / 2);
    wp::options::Rect small = wp::options::place(frame.width / 2, 720, frame.width, frame.height);
    CHECK(small.width == frame.width / 2 && small.x == 0);
    int top = placed.y + frame.padding + frame.title_height;
    int middle = placed.x + frame.width / 2;
    CHECK(wp::options::row_at(frame, placed, middle, top) == 0);
    CHECK(wp::options::row_at(frame, placed, middle, top + frame.row_height * 3 + 1) == 3);
    CHECK(wp::options::row_at(frame, placed, middle, top - 1) == -1);
    CHECK(wp::options::row_at(frame, placed, middle, top + frame.row_height * 12) == -1);
    CHECK(wp::options::row_at(frame, placed, placed.x + 1, top) == -1);
    CHECK(wp::options::row_at(frame, placed, placed.x - 100, top) == -1);

    uint64_t version = 0;
    wp::options::Image image;
    bool visible = false;
    CHECK(!wp::options::take_overlay(version, image, visible));
    wp::options::show_overlay(wp::options::Image{{1, 2, 3, 4}, 2, 2});
    CHECK(wp::options::take_overlay(version, image, visible));
    CHECK(visible && image.width == 2 && image.pixels.size() == 4);
    CHECK(!wp::options::take_overlay(version, image, visible));
    wp::options::hide_overlay();
    CHECK(wp::options::take_overlay(version, image, visible));
    CHECK(!visible && image.pixels.empty());
}

void save_menu_png(const wp::options::Image& image, const std::string& path) {
    std::vector<uint32_t> composed(image.pixels.size());
    for (uint32_t y = 0; y < image.height; y++) {
        for (uint32_t x = 0; x < image.width; x++) {
            size_t index = static_cast<size_t>(y) * image.width + x;
            uint32_t pixel = image.pixels[index];
            uint32_t alpha = pixel >> 24;
            uint32_t back = ((x / 16 + y / 16) % 2) ? 0x3CA0E0 : 0xF0C040;
            uint32_t result = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                uint32_t front = (pixel >> shift) & 0xFF;
                uint32_t behind = (back >> shift) & 0xFF;
                result |= ((front * alpha + behind * (255 - alpha)) / 255) << shift;
            }
            composed[index] = result;
        }
    }
    wp::video::save_png(path.c_str(), composed, image.width, image.height);
}

void test_options_render() {
    wp::settings::load("");
    wp::options::Menu menu;
    menu.set_open(true);
    menu.select(1);
    const char* prefix = std::getenv("WP_MENU_PNG");
    for (int height : {480, 720, 1080}) {
        for (wp::ui::Language language : {wp::ui::Language::English, wp::ui::Language::Spanish}) {
            wp::ui::set_language(language);
            wp::options::Image image = wp::options::render(menu, height);
            wp::options::Layout frame = wp::options::layout(height, wp::settings::keys().size());
            CHECK(image.width == static_cast<uint32_t>(frame.width) && image.height == static_cast<uint32_t>(frame.height));
            CHECK(image.pixels.size() == static_cast<size_t>(image.width) * image.height);
            if (image.pixels.empty()) {
                continue;
            }
            CHECK((image.pixels[0] >> 24) == 0);
            CHECK((image.pixels[image.pixels.size() / 2] >> 24) > 200);
            size_t accent = 0;
            size_t white = 0;
            for (uint32_t pixel : image.pixels) {
                accent += (pixel & 0xFFFFFF) == 0xD4146F;
                white += (pixel & 0xFFFFFF) == 0xFFFFFF;
            }
            CHECK(accent > 50);
            CHECK(white > 100);
            if (prefix) {
                save_menu_png(image, std::string(prefix) + "_" + std::to_string(height) + (language == wp::ui::Language::Spanish ? "_es.png" : "_en.png"));
            }
        }
    }
    wp::ui::set_language(wp::ui::Language::English);
}

}

int main() {
    wp::g_memory = static_cast<uint8_t*>(std::calloc(wp::kMemorySize, 1));
    test_memory();
    test_rotate_and_shift();
    test_arithmetic();
    test_condition_register();
    test_floating_point();
    test_paired_singles();
    test_quantization();
    test_disc_drive();
    test_save_backups();
    test_custom_texture_hash();
    test_custom_texture_names();
    test_custom_texture_index();
    test_gx_logic_ops();
    test_gx_line_point_offsets();
    test_key_names();
    test_key_defaults();
    test_settings_file_keys();
    test_screenshot_names();
    test_live_settings();
    test_options_menu();
    test_options_repeat_and_layout();
    test_options_render();
    test_software_fma();
    std::free(wp::g_memory);
    if (failures == 0) {
        std::puts("all runtime tests passed");
    }
    return failures == 0 ? 0 : 1;
}
