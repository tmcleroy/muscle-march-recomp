#include "wp/options_window.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wp/log.h"
#include "wp/ui_text.h"

namespace wp::options {

namespace {

constexpr uint32_t kPanel = 0x161618;
constexpr uint32_t kSelected = 0x2C2C30;
constexpr uint32_t kSeparator = 0x3A3A3E;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kMuted = 0xA1A1A6;
constexpr uint32_t kAccent = 0xD4146F;
constexpr uint32_t kPanelAlpha = 236;
constexpr char32_t kEllipsis = 0x2026;

enum class Align { Left, Right, Center };

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

std::string matched_font(const char* pattern) {
    std::string command = std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return "";
    }
    std::string path;
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, pipe)) {
        path += buffer;
    }
    pclose(pipe);
    return path;
}

Font load_font(bool bold) {
    std::vector<std::string> candidates = {matched_font(bold ? "sans-serif:weight=180" : "sans-serif")};
    const char* fallbacks[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",      "/Library/Fonts/Arial.ttf",
    };
    candidates.insert(candidates.end(), std::begin(fallbacks), std::end(fallbacks));
    Font font;
    for (const std::string& path : candidates) {
        std::error_code error;
        if (path.empty() || !std::filesystem::is_regular_file(path, error)) {
            continue;
        }
        std::ifstream file(path, std::ios::binary);
        font.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        int offset = stbtt_GetFontOffsetForIndex(font.data.data(), 0);
        if (offset >= 0 && stbtt_InitFont(&font.info, font.data.data(), offset)) {
            font.ok = true;
            log::write("options", "menu font %s", path.c_str());
            return font;
        }
    }
    log::write("options", "no usable font for the options menu");
    return font;
}

const Font& font(bool bold) {
    static const Font regular = load_font(false);
    static const Font semibold = load_font(true);
    return bold && semibold.ok ? semibold : regular;
}

std::u32string decode(const std::string& text) {
    std::u32string result;
    for (size_t i = 0; i < text.size();) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        int length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
        char32_t value = length == 1 ? lead : lead & (0x7F >> length);
        for (int k = 1; k < length && i + k < text.size(); k++) {
            value = (value << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        result.push_back(value);
        i += static_cast<size_t>(length);
    }
    return result;
}

struct Face {
    const Font* font;
    float scale;
    int ascent;
    int descent;
};

Face make_face(bool bold, int height) {
    const Font& chosen = font(bold);
    Face face{&chosen, stbtt_ScaleForMappingEmToPixels(&chosen.info, static_cast<float>(height)), 0, 0};
    int ascent = 0;
    int descent = 0;
    int gap = 0;
    stbtt_GetFontVMetrics(&chosen.info, &ascent, &descent, &gap);
    face.ascent = static_cast<int>(std::lround(ascent * face.scale));
    face.descent = static_cast<int>(std::lround(descent * face.scale));
    return face;
}

float advance(const Face& face, char32_t codepoint, char32_t next) {
    int width = 0;
    int bearing = 0;
    stbtt_GetCodepointHMetrics(&face.font->info, static_cast<int>(codepoint), &width, &bearing);
    int kern = next ? stbtt_GetCodepointKernAdvance(&face.font->info, static_cast<int>(codepoint), static_cast<int>(next)) : 0;
    return (width + kern) * face.scale;
}

float text_width(const Face& face, const std::u32string& text) {
    float width = 0.0f;
    for (size_t i = 0; i < text.size(); i++) {
        width += advance(face, text[i], i + 1 < text.size() ? text[i + 1] : 0);
    }
    return width;
}

struct Canvas {
    int width;
    int height;
    std::vector<uint32_t> pixels;

    void fill(int left, int top, int right, int bottom, uint32_t color) {
        left = std::max(left, 0);
        top = std::max(top, 0);
        right = std::min(right, width);
        bottom = std::min(bottom, height);
        for (int y = top; y < bottom; y++) {
            std::fill(pixels.begin() + static_cast<std::ptrdiff_t>(y) * width + left, pixels.begin() + static_cast<std::ptrdiff_t>(y) * width + right, color);
        }
    }

    void blend(int x, int y, uint32_t color, unsigned coverage) {
        if (x < 0 || y < 0 || x >= width || y >= height || coverage == 0) {
            return;
        }
        uint32_t& target = pixels[static_cast<size_t>(y) * width + x];
        uint32_t result = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            unsigned below = (target >> shift) & 0xFF;
            unsigned above = (color >> shift) & 0xFF;
            unsigned mixed = (below * (255 - coverage) + above * coverage + 127) / 255;
            result |= mixed << shift;
        }
        target = result;
    }

    void text(const Face& face, uint32_t color, const std::string& value, int left, int top, int right, int bottom, Align align, bool ellipsis) {
        if (!face.font->ok) {
            return;
        }
        std::u32string glyphs = decode(value);
        if (ellipsis && text_width(face, glyphs) > right - left) {
            while (!glyphs.empty() && text_width(face, glyphs + kEllipsis) > right - left) {
                glyphs.pop_back();
            }
            glyphs.push_back(kEllipsis);
        }
        float width = text_width(face, glyphs);
        float x = align == Align::Left ? static_cast<float>(left) : align == Align::Right ? right - width : left + (right - left - width) / 2.0f;
        int baseline = top + (bottom - top - (face.ascent - face.descent)) / 2 + face.ascent;
        for (size_t i = 0; i < glyphs.size(); i++) {
            int codepoint = static_cast<int>(glyphs[i]);
            float origin = std::floor(x);
            float shift = x - origin;
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            stbtt_GetCodepointBitmapBoxSubpixel(&face.font->info, codepoint, face.scale, face.scale, shift, 0.0f, &x0, &y0, &x1, &y1);
            int glyph_width = x1 - x0;
            int glyph_height = y1 - y0;
            if (glyph_width > 0 && glyph_height > 0) {
                std::vector<unsigned char> bitmap(static_cast<size_t>(glyph_width) * glyph_height);
                stbtt_MakeCodepointBitmapSubpixel(&face.font->info, bitmap.data(), glyph_width, glyph_height, glyph_width, face.scale, face.scale, shift, 0.0f,
                                                  codepoint);
                for (int gy = 0; gy < glyph_height; gy++) {
                    for (int gx = 0; gx < glyph_width; gx++) {
                        blend(static_cast<int>(origin) + x0 + gx, baseline + y0 + gy, color, bitmap[static_cast<size_t>(gy) * glyph_width + gx]);
                    }
                }
            }
            x += advance(face, glyphs[i], i + 1 < glyphs.size() ? glyphs[i + 1] : 0);
        }
    }
};

uint32_t corner_alpha(int x, int y, int width, int height, int radius) {
    double cx = x + 0.5;
    double cy = y + 0.5;
    double ox = cx < radius ? radius - cx : cx > width - radius ? cx - (width - radius) : 0.0;
    double oy = cy < radius ? radius - cy : cy > height - radius ? cy - (height - radius) : 0.0;
    if (ox <= 0.0 || oy <= 0.0) {
        return kPanelAlpha;
    }
    double coverage = std::clamp(radius - std::sqrt(ox * ox + oy * oy) + 0.5, 0.0, 1.0);
    return static_cast<uint32_t>(kPanelAlpha * coverage + 0.5);
}

}

Image render(const Menu& menu, int client_height) {
    Image image;
    std::vector<Row> rows = menu.rows();
    Layout frame = layout(client_height, rows.size());
    if (frame.width <= 0 || frame.height <= 0 || !font(false).ok) {
        return image;
    }
    Canvas canvas{frame.width, frame.height, std::vector<uint32_t>(static_cast<size_t>(frame.width) * frame.height, kPanel)};
    int unit = frame.unit;
    Face title_face = make_face(true, unit * 3 / 2);
    Face row_face = make_face(false, unit);
    Face small_face = make_face(false, unit * 4 / 5);
    const ui::Text& strings = ui::text();
    int left = frame.padding;
    int right = frame.width - frame.padding;
    canvas.text(title_face, kText, strings.menu_title, left, frame.padding, right, frame.padding + frame.title_height - unit / 2, Align::Left, false);
    int rows_top = frame.padding + frame.title_height;
    canvas.fill(left, rows_top - unit / 2, right, rows_top - unit / 2 + 1, kSeparator);
    int value_width = unit * 9;
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& row = rows[i];
        bool selected = i == menu.selection();
        int top = rows_top + static_cast<int>(i) * frame.row_height;
        int bottom = top + frame.row_height;
        if (selected) {
            canvas.fill(left - unit / 2, top, right + unit / 2, bottom, kSelected);
            canvas.fill(left - unit / 2, top, left - unit / 2 + std::max(3, unit / 5), bottom, kAccent);
        }
        int value_left = right - value_width;
        canvas.text(row_face, selected ? kAccent : kText, row.value, value_left, top, right, bottom, Align::Right, false);
        int label_right = value_left - unit / 2;
        if (!row.live) {
            int note_width = static_cast<int>(std::ceil(text_width(small_face, decode(strings.menu_restart))));
            int note_left = value_left - unit / 2 - note_width;
            canvas.text(small_face, kMuted, strings.menu_restart, note_left - unit, top, value_left - unit / 2, bottom, Align::Right, false);
            label_right = note_left - unit / 2;
        }
        canvas.text(row_face, kText, row.label, left + unit / 2, top, std::max(left + unit, label_right), bottom, Align::Left, true);
    }
    int footer_top = rows_top + static_cast<int>(rows.size()) * frame.row_height;
    canvas.fill(left, footer_top + unit / 2, right, footer_top + unit / 2 + 1, kSeparator);
    canvas.text(small_face, kMuted, strings.menu_hint, left, footer_top + unit / 2, right, footer_top + frame.footer_height, Align::Center, true);
    image.width = static_cast<uint32_t>(frame.width);
    image.height = static_cast<uint32_t>(frame.height);
    image.pixels.resize(canvas.pixels.size());
    int radius = unit * 3 / 4;
    for (int y = 0; y < frame.height; y++) {
        for (int x = 0; x < frame.width; x++) {
            size_t index = static_cast<size_t>(y) * image.width + static_cast<size_t>(x);
            image.pixels[index] = (canvas.pixels[index] & 0xFFFFFF) | (corner_alpha(x, y, frame.width, frame.height, radius) << 24);
        }
    }
    return image;
}

}
