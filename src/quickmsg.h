#pragma once
#include "gnss.h"
#include "storage.h"

#include <string>
#include <vector>

namespace bf {

// Canned messages for a 46-key thumb keyboard held in a cold hand. The
// built-in set covers what a pilot or a hiker actually says on the mesh; any
// of them can be replaced in config.ini with `quickmsg.1 = ...` through
// `quickmsg.12 = ...`, in the same way the Sym layer is corrected there.
constexpr int kMaxQuickMessages = 12;

std::vector<std::string> defaultQuickMessages();
// The defaults with the config file's overrides applied. An override that is
// blank removes that slot; a slot number past the defaults appends.
std::vector<std::string> loadQuickMessages(const Config& config);

// Fills the one placeholder a canned message may carry: `{pos}` becomes this
// station's own coordinate, or an honest "no GNSS fix" when there is none.
// A message that mentions a position it does not have is worse than silence,
// so the placeholder never expands to a stale coordinate.
std::string expandQuickMessage(const std::string& text, const GnssFix& fix);

} // namespace bf
