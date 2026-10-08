#include "wp/features.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

#include "wp/log.h"
#include "wp/movie.h"
#include "wp/settings.h"

namespace wp::features {

namespace {

std::vector<Info>& registry() {
    static std::vector<Info> list;
    return list;
}

std::set<std::string> split(const std::string& text) {
    std::set<std::string> names;
    std::stringstream stream(text);
    std::string name;
    while (std::getline(stream, name, ',')) {
        if (!name.empty() && name != "none") {
            names.insert(name);
        }
    }
    return names;
}

std::set<std::string> resolve() {
    if (movie::replaying()) {
        return split(movie::recorded_features());
    }
    if (const char* forced = std::getenv("WP_FEATURES")) {
        return split(forced);
    }
    std::string preset = settings::text("features.preset", "WP_PRESET");
    uint32_t mask = preset == "enhanced" ? static_cast<uint32_t>(kEnhanced) : 0u;  // pure and custom start from nothing
    std::set<std::string> names;
    for (const Info& info : registry()) {
        std::string key = std::string("features.") + info.name;
        std::string value = settings::text(key.c_str(), nullptr);
        bool on = value.empty() ? (info.presets & mask) != 0 : (value != "0" && value != "off" && value != "false");
        if (on) {
            names.insert(info.name);
        }
    }
    return names;
}

const std::set<std::string>& active() {
    static const std::set<std::string> names = [] {
        std::set<std::string> result = resolve();
        std::string list;
        for (const std::string& name : result) {
            list += (list.empty() ? "" : ",") + name;
        }
        log::write("features", "enabled: %s", list.empty() ? "none (original game)" : list.c_str());
        return result;
    }();
    return names;
}

}  // namespace

Registrar::Registrar(const Info& info) {
    registry().push_back(info);
}

const std::vector<Info>& all() {
    return registry();
}

bool enabled(const char* name) {
    return active().count(name) != 0;
}

std::string resolved() {
    std::string list;
    for (const std::string& name : active()) {
        list += (list.empty() ? "" : ",") + name;
    }
    return list;
}

}  // namespace wp::features
