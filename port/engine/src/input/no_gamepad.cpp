#include "wp/gamepad.h"

namespace wp::gamepad {

bool connected(uint32_t) {
    return false;
}

State poll(uint32_t, bool) {
    return State{};
}

void set_outputs(uint32_t, bool, uint8_t) {}

uint32_t menu_buttons() {
    return 0;
}

}
