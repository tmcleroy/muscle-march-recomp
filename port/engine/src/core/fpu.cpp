#include "wp/cpu.h"

namespace wp {

#if defined(__x86_64__) || defined(__i386__)
bool g_hardware_fma = __builtin_cpu_supports("fma");

__attribute__((target("fma"))) double hardware_fma(double a, double b, double c) {
    return __builtin_fma(a, b, c);
}
#else
// arm64 (Apple Silicon, Linux, Android) always has a fused multiply-add.
bool g_hardware_fma = true;

double hardware_fma(double a, double b, double c) {
    return __builtin_fma(a, b, c);
}
#endif

}
