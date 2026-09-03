#include "simpty.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace bf {

bool openSimPty(int& master, std::string& slavePath, std::string& error) {
    master = -1;
    slavePath.clear();
#if defined(__linux__)
    const int fd = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0) {
        error = std::string("posix_openpt: ") + std::strerror(errno);
        return false;
    }
    if (::grantpt(fd) != 0 || ::unlockpt(fd) != 0) {
        error = std::string("unlockpt: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    const char* name = ::ptsname(fd);
    if (!name) {
        error = "ptsname failed";
        ::close(fd);
        return false;
    }
    slavePath = name;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    master = fd;
    return true;
#else
    error = "the simulator needs a Linux pty";
    return false;
#endif
}

} // namespace bf
