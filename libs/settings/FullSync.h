// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcntl.h>
#include <unistd.h>

namespace noteAppC {

/// Flushes `fd` to the drive. fsync on macOS stops at the drive's cache, so a power cut can still lose
/// what it reported written; F_FULLFSYNC asks the drive to flush, and some filesystems refuse it, so fsync is the
/// fallback there. On Linux this is fsync, as before.
inline int fullSync(int fd)
{
#ifdef __APPLE__
    if (::fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
#endif
    return ::fsync(fd);
}

} // namespace noteAppC
