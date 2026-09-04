#include "marks.h"
#include "meshtastic.h"
#include "strutil.h"

#include <cstdio>
#include <cstdlib>

namespace bf {

std::string Mark::coordText() const { return formatLatLon(latitude, longitude); }

std::string cleanMarkName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        // Tabs and newlines are record separators; other control characters
        // have no glyph on this screen and no business in a place name.
        if (u < 0x20 || u == 0x7F) continue;
        out.push_back(c);
    }
    out = trim(out);
    if (out.size() > kMaxMarkNameBytes) out.resize(kMaxMarkNameBytes);
    return out;
}

std::string formatMarks(const std::vector<Mark>& marks) {
    std::string out =
        "# GNDHOG ZERO marks v1\n"
        "# stamp\tname\tlat\tlon\talt\tsource\n";
    char head[96];
    for (const Mark& m : marks) {
        std::snprintf(head, sizeof(head), "%lld\t", static_cast<long long>(m.stampUtc));
        out += head;
        out += meshEscapeField(m.name);
        std::snprintf(head, sizeof(head), "\t%.7f\t%.7f\t", m.latitude, m.longitude);
        out += head;
        if (m.haveAltitude) out += std::to_string(m.altitudeM);
        out.push_back('\t');
        out += meshEscapeField(m.source);
        out.push_back('\n');
    }
    return out;
}

std::vector<Mark> parseMarks(const std::string& text) {
    std::vector<Mark> out;
    for (const std::string& line : splitLines(text)) {
        if (out.size() >= kMaxMarks) break;
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> fields = splitFields(line, '\t');
        if (fields.size() < 4) continue;

        Mark m;
        m.stampUtc = std::strtoll(fields[0].c_str(), nullptr, 10);
        m.name = cleanMarkName(meshUnescapeField(fields[1]));
        char* end = nullptr;
        m.latitude = std::strtod(fields[2].c_str(), &end);
        if (end == fields[2].c_str()) continue;
        m.longitude = std::strtod(fields[3].c_str(), &end);
        if (end == fields[3].c_str()) continue;
        // A coordinate the planet does not have is a corrupted line, not a
        // place; it is dropped rather than drawn at the edge of the map.
        if (m.latitude < -90.0 || m.latitude > 90.0 ||
            m.longitude < -180.0 || m.longitude > 180.0) {
            continue;
        }
        if (fields.size() > 4 && !fields[4].empty()) {
            m.altitudeM = static_cast<int32_t>(std::strtol(fields[4].c_str(), &end, 10));
            m.haveAltitude = end != fields[4].c_str();
        }
        if (fields.size() > 5) m.source = meshUnescapeField(fields[5]);
        if (m.name.empty()) m.name = "(unnamed)";
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace bf
