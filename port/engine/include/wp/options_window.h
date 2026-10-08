#pragma once

#include <string>

#include "wp/options.h"

namespace wp::options {

bool menu_open();
void toggle_menu(void* window);
std::string menu_key(void* window, unsigned key, bool repeat);
std::string menu_wheel(void* window, int delta);
std::string menu_click(void* window, int x, int y, bool right);
std::string poll_gamepads(void* window);
void refresh_menu(void* window);
Image render(const Menu& menu, int client_height);

}
