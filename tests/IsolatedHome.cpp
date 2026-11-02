// SPDX-License-Identifier: GPL-3.0-or-later
// Linked into every test binary (tests/CMakeLists.txt). Points the XDG homes at
// the build's isolated-home before main() runs, so a test binary started by
// hand, not through ctest, still never touches the real ~/.config, ~/.cache,
// ~/.local/share or ~/.local/state - a direct run once left thousands of history repos there.
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {
const bool isolatedHome = [] {
    const std::string home = NOTEAPPC_TEST_HOME;
    setenv("XDG_CONFIG_HOME", (home + "/config").c_str(), 1);
    setenv("XDG_CACHE_HOME", (home + "/cache").c_str(), 1);
    setenv("XDG_DATA_HOME", (home + "/data").c_str(), 1);
    setenv("XDG_STATE_HOME", (home + "/state").c_str(), 1);
    // No test process reaches a Notolith someone is using: its single-instance sockets are
    // named under a runtime directory of its own, whatever the person's $XDG_RUNTIME_DIR holds.
    // A directory per process, short enough for a unix socket path, removed when it ends.
    static std::string run = [] {
        // A unix socket path is 107 bytes at most, and a name is "/n-" + 16 digits + ".sock" (25)
        // after the directory: a deep $TMPDIR falls back to /tmp.
        const char *tmp = std::getenv("TMPDIR");
        std::string base = tmp && *tmp ? tmp : "/tmp";
        if (base.size() + std::string("/nac-1234567-XXXXXX").size() + 25 > 107)
            base = "/tmp";
        // The directory of a test that was killed is never reached by the atexit below: it names its
        // process, and the next test binary to start removes those whose process is gone.
        std::error_code ignored;
        for (const auto &entry : std::filesystem::directory_iterator(base, ignored)) {
            const std::string name = entry.path().filename().string();
            struct stat info;
            if (name.rfind("nac-", 0) != 0 || ::lstat(entry.path().c_str(), &info) != 0 || !S_ISDIR(info.st_mode)
                || info.st_uid != ::geteuid())
                continue;
            const long pid = std::strtol(name.c_str() + 4, nullptr, 10);
            if (pid > 0 && name.find('-', 4) != std::string::npos && ::kill(static_cast<pid_t>(pid), 0) != 0
                && errno == ESRCH)
                std::filesystem::remove_all(entry.path(), ignored);
        }
        std::string pattern = base + "/nac-" + std::to_string(::getpid()) + "-XXXXXX";
        return mkdtemp(pattern.data()) ? pattern : std::string();
    }();
    if (!run.empty()) {
        setenv("XDG_RUNTIME_DIR", run.c_str(), 1);
        setenv("NOTEAPPC_INSTANCE_SOCKET", (run + "/n").c_str(), 1);
        std::atexit([] {
            std::error_code ignored;
            std::filesystem::remove_all(run, ignored);
        });
    }
    // The desktop's Qt theme and style are not the test's: under KDE,
    // QT_QPA_PLATFORMTHEME=kde loads KWindowSystem, which warns "Could not
    // find any platform plugin" on offscreen and fails the no-warnings tests.
    unsetenv("QT_QPA_PLATFORMTHEME");
    unsetenv("QT_STYLE_OVERRIDE");
    return true;
}();
}
