#pragma once
#include <cctype>
#include <cstdio>
#include <string>

namespace bf {

// The handful of string helpers every module used to carry a private copy of.
// Whitespace is whatever isspace() says it is.
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// "ttyACM0" for "/dev/ttyACM0"; a name with no slash in it is its own base.
inline std::string baseName(const std::string& path) {
    return path.substr(path.find_last_of('/') + 1);
}

// "51.47790, -0.00150": the one way a coordinate is printed on this screen,
// whether it is the receiver's, a node's, or a saved mark's.
inline std::string formatLatLon(double latitude, double longitude) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.5f, %.5f", latitude, longitude);
    return buf;
}

} // namespace bf
