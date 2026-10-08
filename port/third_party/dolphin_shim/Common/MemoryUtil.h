#pragma once

#include <cstddef>
#include <cstdlib>

namespace Common {

inline void* AllocateMemoryPages(size_t size) {
    return std::calloc(1, size);
}

inline void FreeMemoryPages(void* pointer, size_t) {
    std::free(pointer);
}

inline void WriteProtectMemory(void*, size_t, bool = false) {}

inline void UnWriteProtectMemory(void*, size_t, bool = false) {}

}
