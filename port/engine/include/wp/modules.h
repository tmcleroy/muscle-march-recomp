#pragma once

#include <cstdint>

#include "wp/cpu.h"

namespace wp {

struct ModuleFunction {
    uint32_t section;
    uint32_t offset;
    void (*function)(Cpu&);
};

struct ModuleDescriptor {
    const char* name;
    uint32_t identifier;
    uint32_t signature;
    uint32_t section_count;
    uint32_t* bases;
    const ModuleFunction* functions;
    size_t function_count;
    const ModuleFunction* resumes;
    size_t resume_count;
};

extern const ModuleDescriptor* const g_module_table[];
extern const size_t g_module_count;

bool call_module_function(Cpu& c, uint32_t address);
void (*find_module_resume(uint32_t address))(Cpu&);
void describe_loaded_modules(uint32_t address);
uint32_t external_address(uint32_t module_identifier, uint32_t section, uint32_t offset);
const char* module_name_at(uint32_t address);
bool module_loaded(const char* name);

}
