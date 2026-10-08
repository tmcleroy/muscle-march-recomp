#pragma once
// Adding Muscle March to Steam (Linux, e.g. Steam Deck), so it can be played from Game Mode.

#include <string>

// Whether this system can add non-Steam games from here (Linux with Steam installed).
bool can_add_to_steam();

// Writes two desktop entries (Muscle March, straight into the game with --play; Muscle March
// Launcher) and asks the running Steam client to add both. `message` says what happened.
bool add_to_steam(std::string& message);
