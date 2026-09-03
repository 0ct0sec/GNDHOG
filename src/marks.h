#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf {

// A saved place. The car at the trailhead, the launch pad, the last position
// a tracker on a quad reported before its battery went. Marks are local: they
// are never transmitted, and the mesh never learns that one exists.
struct Mark {
    std::string name;
    double latitude = 0.0;
    double longitude = 0.0;
    bool haveAltitude = false;
    int32_t altitudeM = 0;
    int64_t stampUtc = 0;            // wall clock when it was saved
    std::string source;              // "gnss", or the node id it came from

    std::string coordText() const;
};

// Enough places for a week away and few enough to scroll on a thumb keyboard.
constexpr size_t kMaxMarks = 50;
constexpr size_t kMaxMarkNameBytes = 24;

// One tab-separated record per mark, readable with `cat` and copied off the
// device without a parser. Names are escaped so a tab or newline in one cannot
// become a second record.
std::string formatMarks(const std::vector<Mark>& marks);
std::vector<Mark> parseMarks(const std::string& text);

// Trims, clips to kMaxMarkNameBytes, and strips characters that would break
// the record format. Returns an empty string for a name that is nothing but
// whitespace, which the caller treats as "no name given".
std::string cleanMarkName(const std::string& name);

} // namespace bf
