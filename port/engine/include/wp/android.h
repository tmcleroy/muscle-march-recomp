#pragma once
// Android start-up (platform/android.cpp): the app has no command line, working directory or
// console, so this gives the game the same footing as on a computer.

#include <string>
#include <vector>

namespace wp::android {

// Sends stdout and stderr to the system log, moves to the app's own folder, applies env.txt (test
// and debug variables, one NAME=value per line), installs the game files packed into a development
// build and writes the first settings file. Returns the game's arguments: the program name, the
// game files' folder, the watchdog seconds and the save folder. Empty when there are no game files
// (the player was asked for them, or told).
std::vector<std::string> prepare();

// The game's first-start screen: asked to build the game files in `game_folder` when there are
// none (it has the window to itself and returns once they are there, or false when the player
// left). Without one the player is told where to copy the files.
using Setup = bool (*)(const std::string& game_folder);
void set_setup(Setup setup);

// After a crash report: lets the log drain, then hands the signal back to the system's handler.
void finish_crash(int signal);
void keep_system_handler(int signal);

}
