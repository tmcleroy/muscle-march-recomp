#include "wp/hle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "wp/format.h"
#include "wp/function_table.h"
#include "wp/audio_hle.h"
#include "wp/gx_api.h"
#include "wp/input_hle.h"
#include "wp/storage_hle.h"
#include "wp/threads.h"

namespace wp {

char** g_argv = nullptr;

namespace {

constexpr const char* kSilentPrefix = "HleZero_";

struct Replacement {
    const char* name;
    HleFunction function;
};

std::string read_guest_string(uint32_t address) {
    std::string text;
    for (char ch; (ch = static_cast<char>(rd8(address))) != 0 && text.size() < 256; address++) {
        text.push_back(ch);
    }
    return text;
}

void do_nothing(Cpu&) {}

void return_zero(Cpu& c) {
    c.r[3] = 0;
}

void return_one(Cpu& c) {
    c.r[3] = 1;
}

void os_report(Cpu& c) {
    std::fputs(format_guest(c, c.r[3], 4).c_str(), stderr);
}

void os_panic(Cpu& c) {
    std::string message = "PANIC in " + read_guest_string(c.r[3]) + " on line " + std::to_string(c.r[4]) + ": " +
                          format_guest(c, c.r[5], 6);
    fatal_error(message.c_str());
}

// Leaving the game (HOME menu "Wii Menu", power off): there is no system menu,
// so quit the program. Reset restarts it by executing it again with the same arguments.
void quit_to_menu(Cpu&) {
    std::fputs("game returned to the system menu; exiting\n", stderr);
    std::fflush(nullptr);
    std::_Exit(0);
}

void restart(Cpu&) {
    std::fputs("game reset; restarting\n", stderr);
    std::fflush(nullptr);
    if (g_argv && g_argv[0]) {
#ifdef _WIN32
        _execv(g_argv[0], g_argv);
#else
        execv(g_argv[0], g_argv);
#endif
    }
    std::_Exit(0);
}

void switch_fiber(Cpu& c) {
    uint32_t function = c.r[3];
    c.r[1] = c.r[4];
    call(c, function);
}

const Replacement kReplacements[] = {
    {"EXIInit", do_nothing},
    {"EXILock", return_zero},
    {"EXIUnlock", return_one},
    {"EXIProbe", return_zero},
    {"EXIGetID", return_zero},
    {"OSRealModeCall", do_nothing},
    {"OSReport", os_report},
    {"OSPanic", os_panic},
    {"OSLoadContext", load_context},
    {"OSSwitchFiber", switch_fiber},
    {"longjmp", long_jump},
    {"WPADRead", input_hle::wpad_read},
    {"OSReturnToMenu", quit_to_menu},
    {"OSShutdownSystem", quit_to_menu},
    {"OSRestart", restart},
    {"OSResetSystem", restart},
};

}

std::vector<std::pair<const char*, HleFunction>>& registered_handlers() {
    static std::vector<std::pair<const char*, HleFunction>> list;
    return list;
}

HandlerRegistrar::HandlerRegistrar(const char* name, HleFunction function) {
    registered_handlers().emplace_back(name, function);
}

void call_original(Cpu& c, const char* name) {
    const uint32_t address = symbol_address(name);
    for (size_t i = 0; i < g_original_count; i++) {
        if (g_original_table[i].address == address) {
            g_original_table[i].function(c);
            return;
        }
    }
    std::fprintf(stderr, "no original body for %s\n", name);
    std::abort();
}

HleFunction find_replacement(const char* name) {
    if (std::strncmp(name, kSilentPrefix, std::strlen(kSilentPrefix)) == 0) {
        return return_zero;
    }
    for (const Replacement& replacement : kReplacements) {
        if (std::strcmp(replacement.name, name) == 0) {
            return replacement.function;
        }
    }
    for (const input_hle::Handler* table : {input_hle::kHandlers, storage_hle::kHandlers, audio_hle::kHandlers, gx_api::kHandlers}) {
        for (const input_hle::Handler* handler = table; handler->name; handler++) {
            if (std::strcmp(handler->name, name) == 0) {
                return handler->function;
            }
        }
    }
    for (const auto& [handler_name, function] : registered_handlers()) {
        if (std::strcmp(handler_name, name) == 0) {
            return function;
        }
    }
    std::fprintf(stderr, "no runtime handler for replaced function %s\n", name);
    std::abort();
}

}
