#pragma once

#include <cstddef>
#include <functional>

namespace wp {

void run_with_stack(size_t stack_size, const std::function<void()>& work);

}
