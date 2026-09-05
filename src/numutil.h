#pragma once
#include "strutil.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace bf {

// Persisted fields and device readings are whole numbers, not a numeric
// prefix followed by whatever the parser stopped understanding. Leave the
// caller's previous reading alone on failure. Config may request base 0 to
// retain its existing decimal/octal/hex notation.
template <class Integer>
bool parseInteger(const std::string& text, Integer& out, int base = 10) {
    static_assert(std::is_integral<Integer>::value, "integer destination required");
    const std::string value = trim(text);
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    if constexpr (std::is_signed<Integer>::value) {
        const long long parsed = std::strtoll(value.c_str(), &end, base);
        if (errno != 0 || end != value.c_str() + value.size() ||
            parsed < std::numeric_limits<Integer>::min() ||
            parsed > std::numeric_limits<Integer>::max()) return false;
        out = static_cast<Integer>(parsed);
    } else {
        if (value[0] == '-') return false;
        const unsigned long long parsed = std::strtoull(value.c_str(), &end, base);
        if (errno != 0 || end != value.c_str() + value.size() ||
            parsed > std::numeric_limits<Integer>::max()) return false;
        out = static_cast<Integer>(parsed);
    }
    return true;
}

inline bool parseFiniteDouble(const std::string& text, double& out) {
    const std::string value = trim(text);
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno != 0 || end != value.c_str() + value.size() || !std::isfinite(parsed)) return false;
    out = parsed;
    return true;
}

} // namespace bf
