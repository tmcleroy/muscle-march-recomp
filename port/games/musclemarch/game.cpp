#include "wp/game.h"

namespace wp::game {

namespace {

constexpr Description kDescription = {
    "Muscle March",
    "musclemarch",
    "../extracted",
    "nand",
    "settings.ini",
    "textures",
    nullptr,
    nullptr,
    0,
    true,
    -238.0f,  // calibrated against the HOME menu cursor (2026-10-05)
    -88.0f,
    true,  // native audio
};

}

const Description& description() {
    return kDescription;
}

}
