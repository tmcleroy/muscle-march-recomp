#include "wp/game.h"

#include <iterator>

namespace wp::game {

namespace {

constexpr const char* kSidewaysModules[] = {
    "mg102", "mg104", "mg108", "mg109", "mg202", "mg210", "mg212", "mg213", "mg218", "mg221", "mg222", "mg223", "mg224",
    "mg302", "mg404", "mg407", "mg412", "mg415", "mg422", "mg428", "mg430", "mg431", "mg432", "mg433", "mg436", "mg437",
    "mg438", "mg440", "mg441", "mg445", "mg446", "mg503", "mg504", "mg505", "mg507", "mg508", "mg509",
};

constexpr Description kDescription = {
    "Wii Party",
    "wiiparty",
    "games/wiiparty/extracted",
    "games/wiiparty/nand",
    "games/wiiparty/settings.ini",
    "games/wiiparty/textures",
    "boot",
    kSidewaysModules,
    std::size(kSidewaysModules),
};

}

const Description& description() {
    return kDescription;
}

}
