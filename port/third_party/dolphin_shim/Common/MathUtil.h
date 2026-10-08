#pragma once

#include <limits>

namespace MathUtil {

template <typename To, typename From>
constexpr To SaturatingCast(From value) {
    if (value < static_cast<From>(std::numeric_limits<To>::min())) {
        return std::numeric_limits<To>::min();
    }
    if (value > static_cast<From>(std::numeric_limits<To>::max())) {
        return std::numeric_limits<To>::max();
    }
    return static_cast<To>(value);
}

}
