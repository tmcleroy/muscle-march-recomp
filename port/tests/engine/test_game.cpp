#include "wp/function_table.h"
#include "wp/game.h"
#include "wp/modules.h"

namespace wp {
const FunctionEntry g_function_table[1] = {};
const size_t g_function_count = 0;
const FunctionEntry g_resume_table[1] = {};
const FunctionEntry g_original_table[1] = {};
const size_t g_original_count = 0;
const size_t g_resume_count = 0;
const ModuleDescriptor* const g_module_table[1] = {nullptr};
const size_t g_module_count = 0;
const NameEntry g_name_table[1] = {};
const size_t g_name_count = 0;
}

namespace wp::game {

const Description& description() {
    static const Description kDescription = {"Engine tests", "engine_tests", "", "", "", "", nullptr, nullptr, 0};
    return kDescription;
}

}
