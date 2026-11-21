// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace noteAppC {

/// Flushes `fd` to the drive. fsync on macOS stops at the drive's cache, so a power cut can still lose
/// what it reported written; F_FULLFSYNC asks the drive to flush, and some filesystems have no such request (ENOTSUP,
/// ENOTTY, EINVAL), so fsync is the fallback there. On Linux this is fsync, as before.
inline int fullSync(int fd)
{
#ifdef __APPLE__
    if (::fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
    // Only a filesystem that has no such request falls back; a real I/O error must not be hidden by fsync.
    if (errno != ENOTSUP && errno != ENOTTY && errno != EINVAL)
        return -1;
#endif
    return ::fsync(fd);
}

} // namespace noteAppC
