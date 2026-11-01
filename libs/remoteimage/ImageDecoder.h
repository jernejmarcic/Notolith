#pragma once

// Runs notolith-imagedecode under bubblewrap (libs/sandbox: no network, no home, the syscall filter, rlimits)
// and checks what it answers. Nothing here decodes an image in this process, and when the sandbox is not
// there nothing is decoded at all.

#include "Bubblewrap.h"
#include "ImageWire.h"

#include <QString>

namespace noteAppC::remoteimage {

/// Where the helper is: NOTEAPPC_IMAGE_HELPER (development builds only), beside the executable, in the
/// installed libexec directory. Empty when it is nowhere.
QString helperPath();

/// Empty when a decode can run (bubblewrap and the syscall filter and the helper are all there), else the
/// notice to show. Worked out once per process (it builds the command and throws it away), then remembered.
QString decoderUnavailable();

/// The bubblewrap line a decode runs, the same one TeX, pandoc and the notebooks get: tmpfs and /dev/shm
/// sized, 256 tasks, and a cgroup of 1 GiB where the user's systemd answers. Empty program when it cannot be built.
/// The caller closes `filterFD`.
sandbox::Command decoderCommand();

/// Decodes `bytes` (already sniffed as `format`) in a fresh helper process: blocking, for a worker thread.
DecodedImage decodeInSandbox(const QByteArray &bytes, ImageFormat format);

} // namespace noteAppC::remoteimage
