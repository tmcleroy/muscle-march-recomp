#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>  // the activity calls main() under another name

#include "wp/android.h"
#endif

#include "wp/gx_api.h"
#include "wp/big_stack.h"
#include "wp/boot.h"
#include "wp/cpu.h"
#include "wp/disc.h"
#include "wp/dol.h"
#include "wp/features.h"
#include "wp/game.h"
#include "wp/hle.h"
#include "wp/nand.h"
#include "wp/ipc.h"
#include "wp/settings.h"
#include "wp/threads.h"
#include "wp/title.h"
#include "wp/video.h"
#include "wp/watch.h"

namespace {

#ifdef _WIN32
LONG WINAPI report_crash(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    std::fprintf(stderr, "crash: exception %08lx at host address %p (module offset %llx)", record->ExceptionCode, record->ExceptionAddress,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record->ExceptionAddress) - reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr))));
    if (record->NumberParameters >= 2 && (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)) {
        std::fprintf(stderr, ", %s address %p", record->ExceptionInformation[0] ? "writing" : "reading",
                     reinterpret_cast<void*>(record->ExceptionInformation[1]));
    }
    std::fputc(10, stderr);
    wp::print_guest_registers();
    wp::print_call_stack();
    wp::print_thread_stacks();
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void report_crash(int signal) {
    std::fprintf(stderr, "crash: signal %d", signal);
    std::fputc(10, stderr);
    wp::print_guest_registers();
    wp::print_call_stack();
    wp::print_thread_stacks();
    std::fflush(stderr);
#ifdef __ANDROID__
    wp::android::finish_crash(signal);
#else
    std::_Exit(128 + signal);
#endif
}
#endif

}

// --list-features: one line per optional feature (for the launcher), tab-separated:
// name, category, presets (bit 1: enhanced), title, description.
bool list_features(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--list-features") != 0) {
        return false;
    }
    for (const wp::features::Info& info : wp::features::all()) {
        std::printf("%s\t%s\t%u\t%s\t%s\n", info.name, info.category, info.presets, info.title, info.description);
    }
    std::fflush(stdout);
    return true;
}

int run(int argc, char** argv) {
    if (list_features(argc, argv)) {
        return 0;
    }
    wp::settings::load(wp::game::description().settings_file);
    std::string extracted = argc > 1 ? argv[1] : wp::game::description().data_directory;
    int timeout_seconds = argc > 2 ? std::atoi(argv[2]) : 0;

    wp::g_memory = static_cast<uint8_t*>(std::calloc(wp::kMemorySize, 1));
    if (!wp::g_memory) {
        std::fputs("cannot allocate guest memory\n", stderr);
        return 1;
    }
    uint32_t entry = 0;
    const bool title = wp::title::mount(extracted);
    const std::string dol = extracted + (title ? "/main.dol" : "/sys/main.dol");
    if (!wp::load_dol(dol, entry)) {
        std::fprintf(stderr, "cannot load %s\n", dol.c_str());
        return 1;
    }
    if (title) {
        uint8_t game_id[6];
        uint32_t low = wp::title::title_low();
        uint16_t group = wp::title::group_id();
        const uint8_t id[6] = {uint8_t(low >> 24), uint8_t(low >> 16), uint8_t(low >> 8), uint8_t(low), uint8_t(group >> 8), uint8_t(group)};
        std::memcpy(game_id, id, sizeof game_id);
        wp::init_title_memory(game_id, wp::title::ios_version());
    } else if (!wp::init_boot_memory(extracted)) {
        std::fprintf(stderr, "cannot read boot files in %s/sys\n", extracted.c_str());
        return 1;
    }

    std::string nand_root = argc > 3 ? argv[3] : wp::game::description().nand_directory;
    if (!wp::nand::mount(nand_root)) {
        std::fprintf(stderr, "cannot prepare the virtual NAND in %s\n", nand_root.c_str());
        return 1;
    }
    if (title) {
        // ES creates the data directory when a title is installed.
        char data[64];
        std::snprintf(data, sizeof data, "/title/%08x/%08x/data", wp::title::title_high(), wp::title::title_low());
        wp::nand::create_directory(data);
    }
    if (!title && !wp::disc::mount(extracted)) {
        std::fprintf(stderr, "cannot read the disc layout in %s\n", extracted.c_str());
        return 1;
    }

    if (const char* watch = std::getenv("WP_WATCH")) {
        wp::start_watch(static_cast<uint32_t>(std::strtoul(watch, nullptr, 16)));
    }
    if (std::getenv("WP_PROFILE")) {
        wp::start_profiler();
    }
    if (timeout_seconds > 0) {
        std::thread([timeout_seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(timeout_seconds));
            std::fprintf(stderr, "no progress after %d seconds\n", timeout_seconds);
            wp::print_call_stack();
            wp::print_thread_stacks();
            wp::ipc::report();
            wp::print_profile();
            wp::gx_api::print_report();
            std::_Exit(3);
        }).detach();
    }

    wp::video::start();
    wp::Cpu cpu{};
    wp::init_threads(cpu);
    wp::call(cpu, entry);
    std::puts("entry returned");
    return 0;
}

int main(int argc, char** argv) {
#ifdef __ANDROID__
    static std::vector<std::string> arguments = wp::android::prepare();
    if (arguments.empty()) {
        return 1;
    }
    static std::vector<char*> pointers;
    for (std::string& argument : arguments) {
        pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    argc = static_cast<int>(arguments.size());
    argv = pointers.data();
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
        wp::android::keep_system_handler(signal);
    }
#endif
    wp::g_argv = argv;
#ifdef _WIN32
    SetUnhandledExceptionFilter(report_crash);
    return run(argc, argv);
#else
    std::signal(SIGSEGV, report_crash);
    std::signal(SIGBUS, report_crash);
    std::signal(SIGILL, report_crash);
    std::signal(SIGFPE, report_crash);
    int result = 0;
#if defined(__APPLE__) || defined(__ANDROID__)
    // The window lives on this thread (Cocoa's main thread; the thread SDL's activity gave us).
    std::thread([&] {
        wp::run_with_stack(size_t{512} << 20, [&] { result = run(argc, argv); });
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(result);
    }).detach();
    wp::video::run_main_thread();
    // Headless, or the window could not open: the game thread keeps going.
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
#else
    wp::run_with_stack(size_t{512} << 20, [&] { result = run(argc, argv); });
    return result;
#endif
#endif
}
