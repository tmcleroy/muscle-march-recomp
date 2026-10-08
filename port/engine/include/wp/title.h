#pragma once

#include <cstdint>
#include <string>

// A title booted from NAND (WiiWare/channel) instead of a disc: its TMD and
// decrypted contents, served to the game through /dev/es.
namespace wp::title {

bool mount(const std::string& extracted_directory);
bool active();
// The extracted directory the title was mounted from.
const std::string& directory();
uint32_t title_high();
uint32_t title_low();
uint16_t group_id();
uint32_t ios_version();

int32_t es_ioctlv(uint32_t command, uint32_t input_count, uint32_t output_count, uint32_t vectors, bool& handled);

}
