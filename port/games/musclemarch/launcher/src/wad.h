#pragma once
// Checking the player's game file. The launcher only ever reads it: never copies, moves or
// changes it. Muscle March is WiiWare, so the file is a WAD (title WMME).

#include <cstdint>
#include <string>

struct WadCheck {
    enum class Result { Ok, OtherRevision, NotMuscleMarch, NotWad, Unreadable };
    Result result = Result::Unreadable;
    std::string title_id;   // e.g. "00010001574d4d45"
    std::string sha1;
    uint64_t size = 0;
    std::string message;    // for the player
};

// Reads the whole file once (title ID from the ticket, SHA-1 of the file).
WadCheck check_wad(const std::string& path);
