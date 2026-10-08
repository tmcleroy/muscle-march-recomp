#include "wp/dsp_roms.h"

#if defined(_WIN32)
#define WP_ROM_SECTION ".section .rdata,\"dr\"\n"
#elif defined(__APPLE__)
#define WP_ROM_SECTION ".const_data\n"
#else
#define WP_ROM_SECTION ".section .rodata\n"
#endif

#if defined(__APPLE__)
#define WP_ROM_SYMBOL(name) "_" #name
#else
#define WP_ROM_SYMBOL(name) #name
#endif

#define WP_EMBED(name, path)                                                                                   \
    asm(WP_ROM_SECTION ".balign 16\n.globl " WP_ROM_SYMBOL(name) "\n" WP_ROM_SYMBOL(name) ":\n.incbin \"" path \
        "\"\n.globl " WP_ROM_SYMBOL(name##_end) "\n" WP_ROM_SYMBOL(name##_end) ":\n.byte 0\n.text\n")

WP_EMBED(wp_dsp_irom, WP_DSP_IROM_PATH);
WP_EMBED(wp_dsp_coef, WP_DSP_COEF_PATH);

extern "C" const unsigned char wp_dsp_irom[];
extern "C" const unsigned char wp_dsp_irom_end[];
extern "C" const unsigned char wp_dsp_coef[];
extern "C" const unsigned char wp_dsp_coef_end[];

namespace wp::dsp {

RomBlob instruction_rom() {
    return {wp_dsp_irom, static_cast<size_t>(wp_dsp_irom_end - wp_dsp_irom)};
}

RomBlob coefficient_rom() {
    return {wp_dsp_coef, static_cast<size_t>(wp_dsp_coef_end - wp_dsp_coef)};
}

}
