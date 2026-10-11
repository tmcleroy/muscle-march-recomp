// Specialised TEV fragment shaders.
//
// The general shader (kTevFragment in gx_render_gl.cpp) interprets the TEV setup per pixel: every
// input selection, bias, scale and swap is a branch on uniform bits. specialise_tev() rewrites
// its text for one setup (the words of tev_key() in gx_render_common.cpp): the uniforms that
// select the code path become constants and the loops over gradients, indirect stages and TEV
// stages are unrolled, so a compiler folds everything down to the arithmetic that setup needs.
// Only the general shader's own text is used, so the arithmetic cannot drift from it; when the
// general shader changes in a way this file does not know, specialise_tev() fails (returns an
// empty string) and the general shader is used.
//
// The OpenGL renderer calls this at run time. The SDL_GPU renderer takes precompiled shaders
// only, so tools/gen_gpu_shaders.py builds this file as a program (WP_TEV_SPECIALISE_TOOL) and
// compiles its output for the setups listed in games/*/tev_keys.txt.

#include <cstdint>
#include <cstdio>
#include <string>

namespace wp::gx::render {

std::string specialise_tev(const std::string& general, const uint32_t* words, uint32_t count);

namespace {

constexpr uint32_t kKeyHeader = 8;  // as kTevKeyHeader and kTevKeyStage in gx_render_common.h
constexpr uint32_t kKeyStage = 5;

struct Rewriter {
    bool ok = true;

    // Replace every occurrence; the text must be there.
    void replace(std::string& text, const std::string& old, const std::string& replacement) {
        size_t at = text.find(old);
        if (at == std::string::npos) {
            std::fprintf(stderr, "specialised TEV shader: the general shader no longer contains: %s\n", old.c_str());
            ok = false;
            return;
        }
        while (at != std::string::npos) {
            text.replace(at, old.size(), replacement);
            at = text.find(old, at + replacement.size());
        }
    }

    // Replace a loop of main() by `count` copies of its body, each with the counter a constant.
    template <typename Body>
    void unroll(std::string& text, const std::string& header, const std::string& counter, uint32_t count, Body&& per_iteration) {
        static const std::string kEnd = "\n    }\n";
        size_t start = text.find(header);
        size_t end = start == std::string::npos ? start : text.find(kEnd, start + header.size());
        if (end == std::string::npos) {
            std::fprintf(stderr, "specialised TEV shader: loop not found: %s\n", header.c_str());
            ok = false;
            return;
        }
        std::string body = text.substr(start + header.size(), end - start - header.size());
        std::string unrolled;
        for (uint32_t index = 0; index < count; index++) {
            std::string copy = body;
            per_iteration(copy, index);
            unrolled += "    {\n        const " + counter + " = " + std::to_string(index) + (counter[0] == 'u' ? "u;" : ";") + copy + kEnd;
        }
        text.replace(start, end + kEnd.size() - start, unrolled);
    }
};

std::string literal(uint32_t value) {
    return std::to_string(value) + "u";
}

}

std::string specialise_tev(const std::string& general, const uint32_t* words, uint32_t count) {
    static const std::string kMain = "void main() {";
    uint32_t stages = count >= kKeyHeader ? words[0] & 0xFF : 0;
    size_t split = general.find(kMain);
    if (stages < 1 || stages > 16 || count != kKeyHeader + kKeyStage * stages || split == std::string::npos) {
        return {};
    }
    uint32_t alpha_test = words[1];
    uint32_t output = words[2];
    uint32_t fog = words[3];
    uint32_t indirect = words[4];
    uint32_t references = words[5];
    uint32_t scales = words[6];
    uint32_t swaps = words[7];
    uint32_t indirect_stages = (indirect & 7) < 4 ? indirect & 7 : 4;
    const uint32_t* last = words + kKeyHeader + kKeyStage * (stages - 1);
    Rewriter rewrite;
    std::string head = general.substr(0, split);
    std::string main = general.substr(split);

    rewrite.replace(main, "uint count = header.x;", "const uint count = " + literal(stages) + ";");
    rewrite.replace(main, "uint indirect_stages = indirect.x & 7u;", "const uint indirect_stages = " + literal(indirect & 7) + ";");
    rewrite.replace(main, "uint texgens = (indirect.x >> 8u) & 15u;", "const uint texgens = " + literal((indirect >> 8) & 15) + ";");
    rewrite.unroll(main, "    for (int g = 0; g < 8; g++) {", "int g", 8, [](std::string&, uint32_t) {});
    rewrite.unroll(main, "    for (uint j = 0u; j < indirect_stages && j < 4u; j++) {", "uint j", indirect_stages, [&](std::string& body, uint32_t) {
        rewrite.replace(body, "indirect.y", literal(references));
        rewrite.replace(body, "indirect.z", literal(scales & 0xFFFF));
        rewrite.replace(body, "indirect.w", literal(scales >> 16));
    });
    rewrite.unroll(main, "    for (uint i = 0u; i < count; i++) {", "uint i", stages, [&](std::string& body, uint32_t i) {
        const uint32_t* stage = words + kKeyHeader + kKeyStage * i;
        rewrite.replace(body, "stage[i].x", literal(stage[0]));
        rewrite.replace(body, "stage[i].y", literal(stage[1]));
        rewrite.replace(body, "stage[i].z", literal(stage[2]));
        rewrite.replace(body, "stage[i].w", literal(stage[3]));
        rewrite.replace(body, "tevind[i].x", literal(stage[4]));
    });
    rewrite.replace(main, "stage[count - 1u].x", literal(last[0]));
    rewrite.replace(main, "stage[count - 1u].y", literal(last[1]));
    rewrite.replace(main, "header.w", literal(output >> 1));
    rewrite.replace(main, "(header.z & 0x100u) != 0u", output & 1 ? "true" : "false");
    rewrite.replace(main, "uint fog_select = uint(fog_integer.x) & 15u;", "const uint fog_select = " + literal(fog & 15) + ";");
    rewrite.replace(main, "(fog_integer.x & 16) != 0", fog & 16 ? "true" : "false");
    rewrite.replace(main, "(compare >> 16u) & 7u", literal(alpha_test & 7));
    rewrite.replace(main, "(compare >> 19u) & 7u", literal((alpha_test >> 3) & 7));
    rewrite.replace(main, "(compare >> 22u) & 3u", literal((alpha_test >> 6) & 3));
    std::string tables;
    for (uint32_t table = 0; table < 4; table++) {
        tables += (table ? ", " : "") + literal((swaps >> (8 * table)) & 0xFF);
    }
    rewrite.replace(head, "uint order_bits = swaps[table];", "uint order_bits = kSwapTables[table];");
    rewrite.replace(head, "ivec4 apply_swap(", "const uint kSwapTables[4] = uint[4](" + tables + ");\n\nivec4 apply_swap(");
    // Nothing that is part of the key may still be read from the uniforms.
    for (const char* left : {"header.x", "header.w", "stage[", "tevind[", "swaps[", "indirect.", "fog_integer.x"}) {
        bool declaration = left[0] == 's' || left[0] == 't';  // the uniform block itself declares stage[16] and tevind[16]
        if (main.find(left) != std::string::npos || (!declaration && head.find(left) != std::string::npos)) {
            std::fprintf(stderr, "specialised TEV shader: %s is still read; teach specialise_tev about it\n", left);
            rewrite.ok = false;
        }
    }
    return rewrite.ok ? head + main : std::string();
}

}

#ifdef WP_TEV_SPECIALISE_TOOL
// tev_specialise general.glsl output-directory < keys: writes <directory>/<line number>.glsl for
// every key on standard input (hex words, one key per line).
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    std::ifstream file(argv[1]);
    std::stringstream general;
    general << file.rdbuf();
    std::string line;
    for (int number = 0; std::getline(std::cin, line); number++) {
        std::vector<uint32_t> words;
        std::istringstream stream(line);
        for (std::string word; stream >> word;) {
            words.push_back(static_cast<uint32_t>(std::strtoul(word.c_str(), nullptr, 16)));
        }
        std::string text = wp::gx::render::specialise_tev(general.str(), words.data(), static_cast<uint32_t>(words.size()));
        if (text.empty()) {
            std::fprintf(stderr, "cannot specialise key: %s\n", line.c_str());
            return 1;
        }
        std::ofstream(std::string(argv[2]) + "/" + std::to_string(number) + ".glsl") << text;
    }
    return 0;
}
#endif
