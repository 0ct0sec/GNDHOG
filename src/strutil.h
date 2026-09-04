#pragma once
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

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

inline bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The lines of a text, cut at every '\n' with a trailing '\r' dropped from
// each, the way every line-oriented parser here wants them. A final newline
// ends the last line rather than starting an empty one.
inline std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        size_t end = nl == std::string::npos ? text.size() : nl;
        if (end > pos && text[end - 1] == '\r') --end;
        out.push_back(text.substr(pos, end - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

// `text` cut at every `separator`. Always at least one field, so a record
// parser can index the first without looking; empty fields are kept, because
// an NMEA sentence with no fix is mostly made of them.
inline std::vector<std::string> splitFields(const std::string& text, char separator) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t at = text.find(separator, start);
        if (at == std::string::npos) {
            out.push_back(text.substr(start));
            return out;
        }
        out.push_back(text.substr(start, at - start));
        start = at + 1;
    }
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

// "038": a heading or bearing as three whole degrees, with 360 written 000.
inline std::string formatHeading(double degrees) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d", static_cast<int>(degrees + 0.5) % 360);
    return buf;
}

} // namespace bf
