#pragma once
#include <string>

namespace bf {

// Opens a pseudo-terminal pair for the simulators and the self-test. The
// master end comes back non-blocking; the slave's path is what the app opens
// as though it were a serial port. False, with the reason, when the host
// cannot supply one, and `master` is then -1.
bool openSimPty(int& master, std::string& slavePath, std::string& error);

} // namespace bf
