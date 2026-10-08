#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Optional features (docs/FEATURES.md). With every feature off the game runs exactly as on the
// Wii; anything that changes gameplay, controls or presentation is a feature and opt-in.
//
// Settings: [features] preset = pure | enhanced | custom (default pure), and one key per feature
// (1/0, or blank to follow the preset). WP_FEATURES="a,b" (or "none") overrides both, for tests.
// The set is resolved once, at the first query, and fixed for the run. Input movies store it and
// a replay uses the recorded set.
namespace wp::features {

enum Preset : uint32_t {
    kPure = 1u << 0,      // never contains anything; listed for completeness
    kEnhanced = 1u << 1,
};

struct Info {
    const char* name;         // settings key: features.<name>
    const char* title;
    const char* description;
    const char* category;     // controls, gameplay, presentation, quality of life
    uint32_t presets;         // presets that turn it on
};

// Registration (static, from each feature's source file): WP_FEATURE(name, {...}).
struct Registrar {
    explicit Registrar(const Info& info);
};

const std::vector<Info>& all();
bool enabled(const char* name);
// Comma-separated names of the enabled features (recorded in input movies).
std::string resolved();

}  // namespace wp::features

#define WP_FEATURE(id, ...) static ::wp::features::Registrar id##_feature_registrar{::wp::features::Info __VA_ARGS__}
