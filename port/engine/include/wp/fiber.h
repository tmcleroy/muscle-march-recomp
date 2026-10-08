#pragma once

#include <cstddef>

namespace wp::fiber {

using Handle = void*;
using Entry = void (*)(void*);

Handle adopt_current_thread();
Handle current();
Handle create(size_t stack_size, Entry entry, void* parameter);
void switch_to(Handle target);
void destroy(Handle fiber);

}
