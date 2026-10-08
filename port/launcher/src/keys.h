#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

std::string key_event_name(const SDL_KeyboardEvent& event);
std::string mouse_button_name(uint8_t button);
