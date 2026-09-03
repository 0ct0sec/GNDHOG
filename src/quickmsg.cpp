#include "quickmsg.h"
#include "strutil.h"

namespace bf {

std::vector<std::string> defaultQuickMessages() {
    return {
        "OK, all good here",
        "Landed safe",
        "Crashed, going to look for it",
        "Need help at {pos}",
        "Heading back to the car",
        "On my way to you",
        "Stay put, I will come to you",
        "Battery low, going quiet",
        "Copy that",
        "Flying now, off the radio",
    };
}

std::vector<std::string> loadQuickMessages(const Config& config) {
    std::vector<std::string> out = defaultQuickMessages();
    // Slots are numbered from one because that is how a person counts the
    // lines of a file. A missing key keeps the default; a present, blank key
    // is a deliberate deletion.
    for (int slot = 1; slot <= kMaxQuickMessages; ++slot) {
        const std::string key = "quickmsg." + std::to_string(slot);
        if (config.all().find(key) == config.all().end()) continue;
        const std::string value = config.get(key);
        const size_t index = static_cast<size_t>(slot - 1);
        if (index < out.size()) {
            out[index] = value;
        } else if (!value.empty()) {
            out.resize(index + 1);
            out[index] = value;
        }
    }
    std::vector<std::string> kept;
    for (const std::string& text : out) {
        if (!text.empty()) kept.push_back(text);
    }
    return kept;
}

std::string expandQuickMessage(const std::string& text, const GnssFix& fix) {
    const std::string placeholder = "{pos}";
    std::string out = text;
    // Formatted here rather than through GnssFix::coordText(), which answers
    // for the receiver's history ("no fix" until it has ever had one); this
    // placeholder only cares whether there is a fix right now.
    const std::string value = fix.valid ? formatLatLon(fix.latitude, fix.longitude)
                                        : std::string("(no GNSS fix)");
    size_t at = out.find(placeholder);
    while (at != std::string::npos) {
        out.replace(at, placeholder.size(), value);
        at = out.find(placeholder, at + value.size());
    }
    return out;
}

} // namespace bf
