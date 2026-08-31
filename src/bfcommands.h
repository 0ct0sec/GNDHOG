#pragma once
#include <set>
#include <string>
#include <vector>

namespace bf {

// Betaflight's CLI has no server-side completion -- the Configurator does it
// locally -- so the same job is done here: a built-in command list, plus a
// parameter index harvested from whatever `dump`/`diff`/`get` output scrolls
// past. Nothing extra is sent to the FC to build it.
class Completer {
public:
    Completer();

    struct Result {
        std::vector<std::string> candidates;
        std::string commonPrefix;   // longest shared extension of the prefix
        std::string prefix;
    };

    // Completes the word ending at `cursor` using the context of the whole line.
    Result complete(const std::string& line, int cursor) const;

    // Learns `set foo = bar` / `foo = bar` parameter names from FC output.
    void harvest(const std::string& text);
    void addParam(const std::string& name);
    size_t paramCount() const { return params_.size(); }
    const std::set<std::string>& params() const { return params_; }
    const std::vector<std::string>& commands() const { return commands_; }

private:
    std::vector<std::string> commands_;
    std::set<std::string> params_;
    std::vector<std::string> features_;
};

enum class Risk {
    None,
    Writes,      // changes persistent state
    Motors,      // can spin motors -- props-off confirmation
    Destructive, // wipes settings or leaves the CLI unusable
};

struct RiskNote {
    Risk risk = Risk::None;
    std::string message;
};

// Classifies a command line the user is about to send.
RiskNote riskFor(const std::string& line);

// First whitespace-delimited token, lowercased.
std::string commandWord(const std::string& line);

// Pulls "# name: AIR65 C" / "set craft_name = AIR65 C" out of a dump so backups
// can be named the way Betaflight Configurator names them.
std::string craftNameFromDump(const std::string& dump);
// Pulls the board identifier out of a dump (`board_name BETAFPVG473_V2`).
std::string boardNameFromDump(const std::string& dump);

} // namespace bf
