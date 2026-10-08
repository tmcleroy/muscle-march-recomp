#pragma once
// Building the game's files from the player's WAD with their Wii common key.
//
// The WAD's contents are encrypted with a title key, which is itself encrypted with the console's
// common key. The launcher ships no key: the player gives theirs (a key file, or 32 hexadecimal
// digits), and it is accepted only if it really decrypts this WAD (every content then matches
// its SHA-1 in the title's TMD). The WAD is only read. The result, in the player's folder:
//   title.tmd, contents/NN.app   the decrypted contents
//   main.dol                     the game program (content 1, LZ-compressed in the WAD)
//   files/NN/...                 the U8 archives' files, which the game opens by name
//   source.txt                   the WAD's SHA-1 (an install from another file is redone)

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using CommonKey = std::array<uint8_t, 16>;

struct KeySearch {
    bool found = false;
    CommonKey key{};
    std::string message;  // for the player when not found
};

// Finds the common key in what the player supplied: 32 hexadecimal digits (spaces, dashes and a
// 0x prefix allowed), 16 raw bytes, or a key file holding it at any offset.
KeySearch find_common_key(const std::string& wad_path, const std::vector<uint8_t>& supplied);

struct InstallProgress {
    std::atomic<float> fraction{0.0f};
};

// Decrypts, checks and unpacks the WAD into `folder` (built beside it, then moved into place).
bool install_game(const std::string& wad_path, const std::string& wad_sha1, const CommonKey& key, const std::filesystem::path& folder,
                  InstallProgress& progress, std::string& error);

// Whether `folder` holds a complete install of the WAD with this SHA-1.
bool installed(const std::filesystem::path& folder, const std::string& wad_sha1);

std::string key_to_hex(const CommonKey& key);
bool key_from_hex(const std::string& text, CommonKey& key);
