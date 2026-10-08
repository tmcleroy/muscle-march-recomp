// On-screen guides: how to pose and shake with the sticks, drawn in the picture's side pillars.
//
// While racing, each stick has a dial in its own pillar (left stick left, right stick right) with a
// dot where the stick is now, so the guides are always in view. When it's time to act:
//   - ahead of a pose wall (feature dual_stick_poses): the stick's "up" and "down" windows at
//     their current width (they narrow through a stage), the one the wall needs in yellow with a
//     marker on the rim, and "POSE!" bouncing above the dial with an arrow down to it;
//   - in a shake section (race modes 1, 2; feature stick_shaking): a left-right arrow with a dot
//     swinging along it, and "SHAKE!" above.
// The game is never changed: the dials are HUD sprites (wp/hud.h) that the renderer blends over
// the finished picture. They are drawn here, on the game thread, from plain reads of the race
// (race/course_query.h) and the live gamepad, at most once per displayed frame; the words are
// drawn once and only move while they bounce.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "game_features.h"
#include "race/course_query.h"
#include "wp/features.h"
#include "wp/hud.h"
#include "wp/input.h"

WP_FEATURE(on_screen_guides, {"on_screen_guides", "On-screen stick guides",
                              "Dials in the picture's side pillars show your sticks while racing, where to point them for the next wall "
                              "(\"POSE!\"), and when to wiggle them (\"SHAKE!\"). Needs dual-stick poses or stick shaking.",
                              "quality of life", wp::features::kEnhanced});

namespace musclemarch::features {

namespace {

using namespace wp;
using Clock = std::chrono::steady_clock;

// Placement in the game picture: the pillars are the outer 80 of its 640 pixels.
constexpr float kPillarWidth = 0.125f;
constexpr float kDialTop = 0.36f;
constexpr float kCueHeight = 0.5f;      // the word and arrow above a dial, in dial widths
constexpr float kBounce = 0.025f;       // of the picture's height
constexpr float kBounceHz = 2.2f;
constexpr int kLookAhead = 3;           // segments ahead a pose wall shows its guide (as autoplay)
constexpr float kFadeSeconds = 0.18f;
constexpr double kRedrawSeconds = 1.0 / 60.0;
constexpr float kWiggleHz = 3.0f;
constexpr uint32_t kMaxSize = 256;      // dial images in pixels (the renderer scales them up smoothly)

constexpr int kNothing = -1;
constexpr int kRacing = 5;              // just the sticks
constexpr int kShake = 4;               // poses are 0..3

// Arms per pose (docs/GAME.md): {left stick (Nunchuk), right stick (Remote)}, +1 up, -1 down.
constexpr int kPoseArms[4][2] = {{1, 1}, {-1, -1}, {-1, 1}, {1, -1}};

struct Color {
    float r, g, b, a;
};
constexpr Color kPanel{0.04f, 0.04f, 0.08f, 0.62f};
constexpr Color kRim{1.0f, 1.0f, 1.0f, 0.88f};
constexpr Color kWindow{1.0f, 1.0f, 1.0f, 0.3f};
constexpr Color kTarget{1.0f, 0.84f, 0.0f, 0.95f};
constexpr Color kStick{0.18f, 0.9f, 1.0f, 1.0f};
constexpr Color kOutline{0.0f, 0.0f, 0.0f, 0.9f};

// A small canvas with anti-aliased shapes from signed distances (negative inside).
class Canvas {
public:
    Canvas(uint32_t width, uint32_t height) : width_(width), height_(height), pixels_(static_cast<size_t>(width) * height, Color{0, 0, 0, 0}) {}

    // Paints `color` where `distance(x, y)` (in pixels) is negative, with a one-pixel soft edge,
    // within the box from (x0, y0) to (x1, y1).
    template <typename Distance>
    void fill(const Color& color, float x0, float y0, float x1, float y1, Distance distance) {
        uint32_t left = static_cast<uint32_t>(std::clamp(x0 - 1.0f, 0.0f, static_cast<float>(width_)));
        uint32_t right = static_cast<uint32_t>(std::clamp(x1 + 2.0f, 0.0f, static_cast<float>(width_)));
        uint32_t top = static_cast<uint32_t>(std::clamp(y0 - 1.0f, 0.0f, static_cast<float>(height_)));
        uint32_t bottom = static_cast<uint32_t>(std::clamp(y1 + 2.0f, 0.0f, static_cast<float>(height_)));
        for (uint32_t y = top; y < bottom; y++) {
            for (uint32_t x = left; x < right; x++) {
                float d = distance(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                float coverage = std::clamp(0.5f - d, 0.0f, 1.0f);
                if (coverage > 0.0f) {
                    blend(pixels_[static_cast<size_t>(y) * width_ + x], color, coverage);
                }
            }
        }
    }
    // The same within `radius` of (cx, cy).
    template <typename Distance>
    void fill(const Color& color, float cx, float cy, float radius, Distance distance) {
        fill(color, cx - radius, cy - radius, cx + radius, cy + radius, distance);
    }

    options::Image image(float opacity) const {
        options::Image image;
        image.width = width_;
        image.height = height_;
        image.pixels.resize(pixels_.size());
        for (size_t i = 0; i < pixels_.size(); i++) {
            const Color& c = pixels_[i];
            auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            image.pixels[i] = (byte(c.a * opacity) << 24) | (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
        }
        return image;
    }

    float width() const { return static_cast<float>(width_); }
    float height() const { return static_cast<float>(height_); }

private:
    static void blend(Color& under, const Color& over, float coverage) {
        float a = over.a * coverage;
        float out = a + under.a * (1.0f - a);
        if (out <= 0.0f) {
            return;
        }
        under.r = (over.r * a + under.r * under.a * (1.0f - a)) / out;
        under.g = (over.g * a + under.g * under.a * (1.0f - a)) / out;
        under.b = (over.b * a + under.b * under.a * (1.0f - a)) / out;
        under.a = out;
    }

    uint32_t width_;
    uint32_t height_;
    std::vector<Color> pixels_;
};

float circle(float x, float y, float cx, float cy, float r) {
    return std::hypot(x - cx, y - cy) - r;
}

// A pie slice of radius r around `center` degrees (0 = up, clockwise), `half` degrees each side.
float wedge(float x, float y, float cx, float cy, float r, float center, float half) {
    float dx = x - cx;
    float dy = y - cy;
    float distance = std::hypot(dx, dy);
    float angle = std::atan2(dx, -dy) * 57.29578f;
    float off = std::fabs(std::remainder(angle - center, 360.0f));
    float outside_angle = (off - half) * 0.017453f * distance;  // arc length past the edge
    return std::max(distance - r, outside_angle);
}

// A capsule from (x0, y0) to (x1, y1), `width` across.
float segment(float x, float y, float x0, float y0, float x1, float y1, float width) {
    float vx = x1 - x0;
    float vy = y1 - y0;
    float t = std::clamp(((x - x0) * vx + (y - y0) * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
    return std::hypot(x - (x0 + t * vx), y - (y0 + t * vy)) - width * 0.5f;
}

void dot(Canvas& canvas, float x, float y, float r, const Color& color) {
    float outline = std::max(1.5f, r * 0.28f);
    canvas.fill(kOutline, x, y, r + outline, [&](float px, float py) { return circle(px, py, x, y, r + outline); });
    canvas.fill(color, x, y, r, [&](float px, float py) { return circle(px, py, x, y, r); });
}

// Block capitals as strokes in a box 0.6 wide and 1 high (y down); "!" is 0.2 wide. Each stroke is
// a polyline; a dot is a stroke of one point.
struct Glyph {
    float width;
    std::vector<std::vector<std::pair<float, float>>> strokes;
};

const Glyph& glyph(char c) {
    static const Glyph kP{0.6f, {{{0, 1}, {0, 0}, {0.45f, 0}, {0.6f, 0.13f}, {0.6f, 0.37f}, {0.45f, 0.5f}, {0, 0.5f}}}};
    static const Glyph kO{0.6f, {{{0.15f, 0}, {0.45f, 0}, {0.6f, 0.15f}, {0.6f, 0.85f}, {0.45f, 1}, {0.15f, 1}, {0, 0.85f}, {0, 0.15f}, {0.15f, 0}}}};
    static const Glyph kS{0.6f, {{{0.6f, 0.1f}, {0.47f, 0}, {0.13f, 0}, {0, 0.12f}, {0, 0.37f}, {0.13f, 0.5f}, {0.47f, 0.5f}, {0.6f, 0.62f},
                                  {0.6f, 0.88f}, {0.47f, 1}, {0.13f, 1}, {0, 0.9f}}}};
    static const Glyph kE{0.6f, {{{0.6f, 0}, {0, 0}, {0, 1}, {0.6f, 1}}, {{0, 0.5f}, {0.45f, 0.5f}}}};
    static const Glyph kH{0.6f, {{{0, 0}, {0, 1}}, {{0.6f, 0}, {0.6f, 1}}, {{0, 0.5f}, {0.6f, 0.5f}}}};
    static const Glyph kA{0.6f, {{{0, 1}, {0.3f, 0}, {0.6f, 1}}, {{0.12f, 0.64f}, {0.48f, 0.64f}}}};
    static const Glyph kK{0.6f, {{{0, 0}, {0, 1}}, {{0.6f, 0}, {0, 0.58f}}, {{0.22f, 0.4f}, {0.6f, 1}}}};
    static const Glyph kBang{0.2f, {{{0.1f, 0}, {0.1f, 0.62f}}, {{0.1f, 0.95f}}}};
    switch (c) {
    case 'P': return kP;
    case 'O': return kO;
    case 'S': return kS;
    case 'E': return kE;
    case 'H': return kH;
    case 'A': return kA;
    case 'K': return kK;
    default: return kBang;
    }
}

// A word over an arrow pointing down (to the dial below), centred in a canvas.
options::Image cue(uint32_t width, uint32_t height, const char* word, float opacity) {
    Canvas canvas(width, height);
    float w = canvas.width();
    float h = canvas.height();
    constexpr float kSpacing = 0.24f;
    float units = 0.0f;
    for (const char* c = word; *c; c++) {
        units += glyph(*c).width + (c[1] ? kSpacing : 0.0f);
    }
    float size = std::min(h * 0.5f, w * 0.88f / units);  // glyph height in pixels
    float stroke = size * 0.2f;
    float outline = stroke + size * 0.14f;
    float x = (w - units * size) * 0.5f;
    float top = h * 0.06f;
    std::vector<float> starts;
    for (const char* c = word; *c; c++) {
        starts.push_back(x);
        x += (glyph(*c).width + kSpacing) * size;
    }
    // The text's distance: the nearest stroke of any glyph.
    auto text = [&](float px, float py, float width_) {
        float d = 1e9f;
        size_t i = 0;
        for (const char* c = word; *c; c++, i++) {
            for (const auto& stroke_points : glyph(*c).strokes) {
                for (size_t k = 0; k < stroke_points.size(); k++) {
                    float ax = starts[i] + stroke_points[k].first * size;
                    float ay = top + stroke_points[k].second * size;
                    const auto& next = stroke_points[k + 1 < stroke_points.size() ? k + 1 : k];
                    float bx = starts[i] + next.first * size;
                    float by = top + next.second * size;
                    d = std::min(d, (ax == bx && ay == by) ? std::hypot(px - ax, py - ay) - width_ * 0.6f : segment(px, py, ax, ay, bx, by, width_));
                }
            }
        }
        return d;
    };
    canvas.fill(kOutline, 0, 0, w, top + size + outline, [&](float px, float py) { return text(px, py, outline); });
    canvas.fill(kTarget, 0, 0, w, top + size + stroke, [&](float px, float py) { return text(px, py, stroke); });
    // The arrow, pointing down at the dial.
    float cx = w * 0.5f;
    float arrow_top = top + size + size * 0.25f;
    float tip = h - stroke;
    float head = std::min(size * 0.45f, (tip - arrow_top) * 0.6f);
    auto arrow = [&](float px, float py, float width_) {
        float d = segment(px, py, cx, arrow_top, cx, tip, width_);
        d = std::min(d, segment(px, py, cx - head, tip - head, cx, tip, width_));
        return std::min(d, segment(px, py, cx + head, tip - head, cx, tip, width_));
    };
    canvas.fill(kOutline, 0, arrow_top - outline, w, h, [&](float px, float py) { return arrow(px, py, outline); });
    canvas.fill(kTarget, 0, arrow_top - stroke, w, h, [&](float px, float py) { return arrow(px, py, stroke); });
    return canvas.image(opacity);
}

void dial_face(Canvas& canvas, float c, float r, float line) {
    canvas.fill(kPanel, c, c, r, [&](float x, float y) { return circle(x, y, c, c, r); });
    canvas.fill(kRim, c, c, r + line, [&](float x, float y) { return std::fabs(circle(x, y, c, c, r)) - line * 0.5f; });
}

// The stick's dial; side -1 = left stick (outward = left), +1 = right stick. With a target arm,
// the up and down windows, the one the wall needs in yellow and a marker on the rim.
options::Image pose_dial(uint32_t size, int side, int target_arm, bool windows, float stick_x, float stick_y, float slice, float opacity) {
    Canvas canvas(size, size);
    float s = canvas.width();
    float c = s * 0.5f;
    float r = s * 0.36f;
    float line = std::max(1.5f, s * 0.022f);
    dial_face(canvas, c, r, line);
    if (windows) {
        float lean = kPoseLean * static_cast<float>(side);
        float centers[2] = {lean, 180.0f - lean};  // up, down
        for (int i = 0; i < 2; i++) {
            bool hot = (i == 0 && target_arm > 0) || (i == 1 && target_arm < 0);
            canvas.fill(hot ? kTarget : kWindow, c, c, r, [&](float x, float y) { return wedge(x, y, c, c, r - line * 1.5f, centers[i], slice * 0.5f); });
        }
        if (target_arm != 0) {
            float angle = (target_arm > 0 ? centers[0] : centers[1]) * 0.017453f;
            float rim = r + s * 0.075f;
            dot(canvas, c + rim * std::sin(angle), c - rim * std::cos(angle), s * 0.045f, kTarget);
        }
    }
    // The dead zone, then the stick.
    float dead = r * kStickDeadZone;
    canvas.fill(Color{1, 1, 1, 0.22f}, c, c, dead + line, [&](float x, float y) { return std::fabs(circle(x, y, c, c, dead)) - line * 0.3f; });
    float reach = r - s * 0.05f;
    dot(canvas, c + stick_x * reach, c + stick_y * reach, s * 0.06f, kStick);
    return canvas.image(opacity);
}

options::Image wiggle_dial(uint32_t size, float phase, float opacity) {
    Canvas canvas(size, size);
    float s = canvas.width();
    float c = s * 0.5f;
    float r = s * 0.36f;
    float line = std::max(1.5f, s * 0.022f);
    dial_face(canvas, c, r, line);
    float arm = r * 0.62f;
    float width = s * 0.05f;
    float head = s * 0.09f;
    canvas.fill(kTarget, c, c, arm + width, [&](float x, float y) {
        float d = segment(x, y, c - arm, c, c + arm, c, width);
        for (int side : {-1, 1}) {
            float tip = c + static_cast<float>(side) * arm;
            float back = tip - static_cast<float>(side) * head;
            d = std::min(d, segment(x, y, tip, c, back, c - head, width));
            d = std::min(d, segment(x, y, tip, c, back, c + head, width));
        }
        return d;
    });
    dot(canvas, c + std::sin(phase) * arm, c, s * 0.06f, kStick);
    return canvas.image(opacity);
}

struct Guide {
    float opacity = 0.0f;   // the dials (racing)
    float cue = 0.0f;       // the word and the windows (a wall or a shake section)
    int showing = kNothing; // kRacing, a pose (0..3) or kShake
    uint64_t shown = 0;     // signature of the sprites last shown
    Clock::time_point last{};
    bool started = false;
};

Guide g_guide;

// What to show now: kNothing, kRacing, a pose (0..3) or kShake.
int wanted() {
    uint32_t race = course_query::race();
    if (!race || rd32(race + course_query::kPhase) != 6) {
        return kNothing;
    }
    int32_t state = static_cast<int32_t>(rd32(race + course_query::kState));
    // State 1 is racing (0 the countdown, 2 a rival's slow-motion crash, which shows no pillars).
    if (state != 1 || rd32(race + course_query::kCrashState) != 0 || rd32(race + course_query::kIntroRunning) != 0) {
        return kNothing;
    }
    int32_t mode = static_cast<int32_t>(rd32(race + course_query::kMode));
    static const bool force_wiggle = std::getenv("WP_GUIDE_TEST") && std::string(std::getenv("WP_GUIDE_TEST")) == "wiggle";  // tests
    if ((mode == 1 || mode == 2 || force_wiggle) && stick_shaking()) {
        return kShake;
    }
    int pose = dual_stick_poses() ? course_query::next_pose(race, kLookAhead) : -1;
    return pose >= 0 ? pose : kRacing;
}

void update(uint32_t channel, input::Sample& sample) {
    static const bool on = wp::features::enabled("on_screen_guides");
    if (!on || channel != 0) {
        return;
    }
    Clock::time_point now = Clock::now();
    double since = g_guide.started ? std::chrono::duration<double>(now - g_guide.last).count() : kRedrawSeconds;
    if (since < kRedrawSeconds) {
        return;
    }
    g_guide.started = true;
    g_guide.last = now;
    const gamepad::State& pad = input::last_gamepad(0);
    static const bool test = std::getenv("WP_GUIDE_TEST") != nullptr;  // scripted runs: no gamepad is read
    int want = pad.connected || test ? wanted() : kNothing;
    static const bool debug = std::getenv("WP_GUIDE_DEBUG") != nullptr;
    static int logged = -2;
    if (debug && want != logged) {
        logged = want;
        std::fprintf(stderr, "guide: %d\n", want);
    }

    // The dials fade in while racing; the cue (word and windows) fades in on its own, and switching
    // what it shows fades through nothing.
    float step = static_cast<float>(since) / kFadeSeconds;
    g_guide.opacity = want == kNothing ? std::max(0.0f, g_guide.opacity - step) : std::min(1.0f, g_guide.opacity + step);
    bool cue_wanted = want >= 0 && want != kRacing;
    if (cue_wanted && want == g_guide.showing) {
        g_guide.cue = std::min(1.0f, g_guide.cue + step);
    } else {
        g_guide.cue = std::max(0.0f, g_guide.cue - step);
        if (g_guide.cue <= 0.0f) {
            g_guide.showing = want;
        }
    }
    if (g_guide.opacity <= 0.0f) {
        hud::show({});
        g_guide.shown = 0;
        return;
    }

    uint32_t picture_width = 0;
    uint32_t picture_height = 0;
    hud::picture_size(picture_width, picture_height);
    if (picture_width == 0 || picture_height == 0) {
        picture_width = 1280;
        picture_height = 720;
    }
    float aspect = static_cast<float>(picture_width) / static_cast<float>(picture_height);
    uint32_t size = std::clamp<uint32_t>(static_cast<uint32_t>(kPillarWidth * static_cast<float>(picture_width)), 48, kMaxSize);
    float dial_height = kPillarWidth * aspect;  // a square, as a fraction of the picture's height
    float seconds = std::chrono::duration<float>(now.time_since_epoch()).count();
    bool shaking = g_guide.showing == kShake;
    int pose = g_guide.showing >= 0 && g_guide.showing < 4 ? g_guide.showing : -1;

    std::vector<hud::Sprite> sprites;
    uint64_t signature = static_cast<uint64_t>(size) * 1000003u;  // what is shown, to skip unchanged frames
    auto mix = [&](uint64_t value) { signature = (signature ^ value) * 1099511628211ull; };
    for (int i = 0; i < 2; i++) {
        int side = i == 0 ? -1 : 1;
        hud::Sprite sprite;
        sprite.x = i == 0 ? 0.0f : 1.0f - kPillarWidth;
        sprite.y = kDialTop;
        sprite.width = kPillarWidth;
        sprite.height = dial_height;
        if (shaking && g_guide.cue > 0.0f) {
            float phase = std::fmod(seconds * kWiggleHz, 1.0f) * 6.2831853f + (side > 0 ? 3.14159f : 0.0f);
            sprite.image = wiggle_dial(size, phase, g_guide.opacity);
            mix(static_cast<uint64_t>(phase * 1000.0f) + 7);
        } else {
            float x = side < 0 ? pad.left_x : pad.right_x;
            float y = side < 0 ? pad.left_y : pad.right_y;
            static const bool follow_autoplay = std::getenv("WP_GUIDE_TEST") && std::string(std::getenv("WP_GUIDE_TEST")) == "autoplay";
            if (follow_autoplay) {
                // Tests and screenshots: the sticks where a player would hold them for autoplay's pose.
                uint32_t up = side < 0 ? input::kNunchukPointUp : input::kRemotePointUp;
                uint32_t down = side < 0 ? input::kNunchukPointDown : input::kRemotePointDown;
                uint32_t buttons = input::last_sample(0).buttons | sample.buttons;  // after autoplay's transform
                int arm_now = (buttons & up) ? 1 : (buttons & down) ? -1 : 0;
                float lean = kPoseLean * 0.017453f;
                x = arm_now ? static_cast<float>(side) * std::sin(lean) * 0.95f : 0.0f;
                y = arm_now ? -static_cast<float>(arm_now) * std::cos(lean) * 0.95f : 0.0f;
            }
            float length = std::hypot(x, y);
            if (length > 1.0f) {
                x /= length;
                y /= length;
            }
            int arm = pose >= 0 ? kPoseArms[pose][i] : 0;
            bool windows = pose >= 0 && g_guide.cue > 0.5f;
            // Redrawn only when something on it changed.
            float slice = pose_slice_degrees();
            auto q = [](float v, float step_) { return static_cast<int64_t>(std::lround(v / step_)); };
            int64_t key = ((((((q(x, 1.0f / 128) * 512 + q(y, 1.0f / 128)) * 1024 + q(slice, 0.25f)) * 128 + q(g_guide.opacity, 1.0f / 64)) * 4 + (arm + 1)) * 2 +
                            (windows ? 1 : 0)) * 1024 + static_cast<int64_t>(size)) * 2 + i;
            static int64_t cached_key[2] = {-1, -1};
            static options::Image cached[2];
            if (cached_key[i] != key) {
                cached_key[i] = key;
                cached[i] = pose_dial(size, side, arm, windows, x, y, slice, g_guide.opacity);
            }
            sprite.image = cached[i];
            mix(static_cast<uint64_t>(key));
        }
        sprites.push_back(std::move(sprite));
    }
    // The word above each dial, bouncing (drawn once per word and size; only its place changes).
    if (g_guide.cue > 0.0f && (shaking || pose >= 0)) {
        const char* word = shaking ? "SHAKE!" : "POSE!";
        uint32_t cue_height = static_cast<uint32_t>(static_cast<float>(size) * kCueHeight);
        static std::string cached_word;
        static uint32_t cached_size = 0;
        static float cached_opacity = -1.0f;
        static options::Image cached_cue;
        float opacity = g_guide.cue * g_guide.opacity;
        if (cached_word != word || cached_size != size || std::fabs(cached_opacity - opacity) > 1.0f / 64) {
            cached_word = word;
            cached_size = size;
            cached_opacity = opacity;
            cached_cue = cue(size, cue_height, word, opacity);
        }
        float bounce = std::fabs(std::sin(seconds * kBounceHz * 3.14159f)) * kBounce;
        float height = dial_height * kCueHeight;
        for (int i = 0; i < 2; i++) {
            hud::Sprite sprite;
            sprite.x = i == 0 ? 0.0f : 1.0f - kPillarWidth;
            sprite.y = kDialTop - height - bounce;
            sprite.width = kPillarWidth;
            sprite.height = height;
            sprite.image = cached_cue;
            mix(static_cast<uint64_t>(sprite.y * 100000.0f) + static_cast<uint64_t>(opacity * 64.0f) * 3);
            sprites.push_back(std::move(sprite));
        }
    }
    if (debug) {
        static double total = 0.0;
        static int count = 0;
        total += std::chrono::duration<double>(Clock::now() - now).count();
        if (++count == 120) {
            std::fprintf(stderr, "guide: %u px dials, %.2f ms per frame\n", size, total / count * 1000.0);
            total = 0.0;
            count = 0;
        }
    }
    if (signature != g_guide.shown) {
        g_guide.shown = signature;
        hud::show(std::move(sprites));
    }
}

struct Install {
    Install() { input::add_transform(&update); }
} g_install;

}  // namespace

}  // namespace musclemarch::features
