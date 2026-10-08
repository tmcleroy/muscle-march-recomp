#include "wp/modules.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "wp/log.h"

namespace wp {

namespace {

constexpr uint32_t kModuleListHead = 0x800030C8;
constexpr uint32_t kInfoNext = 0x04;
constexpr uint32_t kInfoIdentifier = 0x00;
constexpr uint32_t kInfoSectionCount = 0x0C;
constexpr uint32_t kInfoSectionTable = 0x10;
constexpr uint32_t kHeaderBssSize = 0x20;
constexpr uint32_t kSectionEntrySize = 8;
constexpr uint32_t kMaxModules = 64;
constexpr uint32_t kFnvOffset = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

uint32_t mix(uint32_t hash, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        hash = (hash ^ ((value >> shift) & 0xFF)) * kFnvPrime;
    }
    return hash;
}

uint32_t section_table(uint32_t info) {
    uint32_t table = rd32(info + kInfoSectionTable);
    return table < 0x80000000 ? info + table : table;
}

uint32_t signature_of(uint32_t info) {
    uint32_t count = rd32(info + kInfoSectionCount);
    uint32_t table = section_table(info);
    uint32_t hash = mix(kFnvOffset, rd32(info + kInfoIdentifier));
    hash = mix(hash, count);
    for (uint32_t i = 0; i < count; i++) {
        hash = mix(hash, rd32(table + i * kSectionEntrySize + 4));
    }
    return mix(hash, rd32(info + kHeaderBssSize));
}

const ModuleDescriptor* find_descriptor(uint32_t signature) {
    for (size_t i = 0; i < g_module_count; i++) {
        if (g_module_table[i]->signature == signature) {
            return g_module_table[i];
        }
    }
    return nullptr;
}

void update_bases(const ModuleDescriptor& descriptor, uint32_t info) {
    uint32_t table = section_table(info);
    for (uint32_t i = 0; i < descriptor.section_count; i++) {
        descriptor.bases[i] = rd32(table + i * kSectionEntrySize) & ~1u;
    }
}

const ModuleFunction* find_function(const ModuleDescriptor& descriptor, uint32_t section, uint32_t offset) {
    for (size_t i = 0; i < descriptor.function_count; i++) {
        if (descriptor.functions[i].section == section && descriptor.functions[i].offset == offset) {
            return &descriptor.functions[i];
        }
    }
    return nullptr;
}

}

bool call_module_function(Cpu& c, uint32_t address) {
    uint32_t info = rd32(kModuleListHead);
    for (uint32_t guard = 0; info != 0 && guard < kMaxModules; guard++, info = rd32(info + kInfoNext)) {
        const ModuleDescriptor* descriptor = find_descriptor(signature_of(info));
        if (!descriptor) {
            continue;
        }
        update_bases(*descriptor, info);
        for (uint32_t section = 1; section < descriptor->section_count; section++) {
            uint32_t size = rd32(section_table(info) + section * kSectionEntrySize + 4);
            uint32_t base = descriptor->bases[section];
            if (base != 0 && address >= base && address < base + size) {
                const ModuleFunction* function = find_function(*descriptor, section, address - base);
                if (function) {
                    function->function(c);
                    return true;
                }
            }
        }
    }
    return false;
}

void (*find_module_resume(uint32_t address))(Cpu&) {
    uint32_t section = address >> 24;
    uint32_t offset = address & 0xFFFFFF;
    void (*found)(Cpu&) = nullptr;
    int matches = 0;
    uint32_t info = rd32(kModuleListHead);
    for (uint32_t guard = 0; info != 0 && guard < kMaxModules; guard++, info = rd32(info + kInfoNext)) {
        const ModuleDescriptor* descriptor = find_descriptor(signature_of(info));
        if (!descriptor) {
            continue;
        }
        update_bases(*descriptor, info);
        for (size_t i = 0; i < descriptor->resume_count; i++) {
            if (descriptor->resumes[i].section == section && descriptor->resumes[i].offset == offset) {
                found = descriptor->resumes[i].function;
                matches++;
                break;
            }
        }
    }
    if (matches > 1) {
        std::fprintf(stderr, "resume address %08x matches %d loaded modules", address, matches);
        std::fputc(10, stderr);
        return nullptr;
    }
    return found;
}

uint32_t external_address(uint32_t module_identifier, uint32_t section, uint32_t offset) {
    uint32_t info = rd32(kModuleListHead);
    for (uint32_t guard = 0; info != 0 && guard < kMaxModules; guard++, info = rd32(info + kInfoNext)) {
        if (rd32(info + kInfoIdentifier) == module_identifier) {
            return (rd32(section_table(info) + section * kSectionEntrySize) & ~1u) + offset;
        }
    }
    return 0;
}

void describe_loaded_modules(uint32_t address) {
    std::fprintf(stderr, "address %08x, loaded modules:\n", address);
    uint32_t info = rd32(kModuleListHead);
    for (uint32_t guard = 0; info != 0 && guard < kMaxModules; guard++, info = rd32(info + kInfoNext)) {
        uint32_t count = rd32(info + kInfoSectionCount);
        uint32_t table = section_table(info);
        const char* name = "?";
        for (size_t m = 0; m < g_module_count; m++) {
            if (g_module_table[m]->identifier == rd32(info + kInfoIdentifier)) {
                name = g_module_table[m]->name;
            }
        }
        std::fprintf(stderr, "  module %s id %u at %08x signature %08x, %u sections:", name, rd32(info + kInfoIdentifier), info,
                     signature_of(info), count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t base = rd32(table + i * kSectionEntrySize) & ~1u;
            uint32_t size = rd32(table + i * kSectionEntrySize + 4);
            std::fprintf(stderr, " [%u]%08x+%x", i, base, size);
            if (address >= base && address < base + size) {
                std::fprintf(stderr, "<-contains %08x (section %u offset %x)", address, i, address - base);
            }
        }
        std::fputc(10, stderr);
    }
}

namespace log {

void watch_modules() {
    if (!enabled()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next = Clock::now();
    static std::string previous;
    Clock::time_point now = Clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::milliseconds(100);
    std::string current;
    uint32_t info = rd32(kModuleListHead);
    for (uint32_t guard = 0; info != 0 && guard < kMaxModules; guard++, info = rd32(info + kInfoNext)) {
        const char* name = "unknown";
        for (size_t m = 0; m < g_module_count; m++) {
            if (g_module_table[m]->identifier == rd32(info + kInfoIdentifier)) {
                name = g_module_table[m]->name;
            }
        }
        current += current.empty() ? "" : " ";
        current += name;
    }
    if (current != previous) {
        write("module", "loaded modules: %s", current.empty() ? "(none)" : current.c_str());
        previous = current;
    }
}

}

const char* module_name_at(uint32_t address) {
    if (address >= 0x80000000u) {
        return "dol";
    }
    const char* found = nullptr;
    uint32_t info = rd32(kModuleListHead);
    for (uint32_t guard = 0; info != 0 && guard < kMaxModules; guard++, info = rd32(info + kInfoNext)) {
        const char* name = "?";
        for (size_t m = 0; m < g_module_count; m++) {
            if (g_module_table[m]->identifier == rd32(info + kInfoIdentifier)) {
                name = g_module_table[m]->name;
            }
        }
        if (found != nullptr) {
            return "?";
        }
        found = name;
    }
    return found != nullptr ? found : "?";
}


bool module_loaded(const char* name) {
    uint32_t info = rd32(kModuleListHead);
    for (uint32_t guard = 0; info != 0 && guard < kMaxModules; guard++, info = rd32(info + kInfoNext)) {
        uint32_t identifier = rd32(info + kInfoIdentifier);
        for (size_t m = 0; m < g_module_count; m++) {
            if (g_module_table[m]->identifier == identifier && std::strcmp(g_module_table[m]->name, name) == 0) {
                return true;
            }
        }
    }
    return false;
}
}
