#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace iiCalendar::detail {
inline std::int64_t add(std::int64_t a, std::int64_t b) {
    constexpr auto max = std::numeric_limits<std::int64_t>::max(),
                   min = std::numeric_limits<std::int64_t>::min();
    if ((b > 0 && a > max - b) || (b < 0 && a < min - b))
        throw std::overflow_error("time addition overflow");
    return a + b;
}
inline std::int64_t sub(std::int64_t a, std::int64_t b) {
    constexpr auto max = std::numeric_limits<std::int64_t>::max(),
                   min = std::numeric_limits<std::int64_t>::min();
    if ((b < 0 && a > max + b) || (b > 0 && a < min + b))
        throw std::overflow_error("time subtraction overflow");
    return a - b;
}
inline std::int64_t floorDiv(std::int64_t a, std::int64_t positiveDivisor) {
    return a / positiveDivisor - (a % positiveDivisor < 0 ? 1 : 0);
}
inline std::int64_t mod(std::int64_t a, std::int64_t positiveDivisor) {
    auto m = a % positiveDivisor;
    return m < 0 ? m + positiveDivisor : m;
}
} // namespace iiCalendar::detail
