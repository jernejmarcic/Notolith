// Linked into every test binary (tests/CMakeLists.txt). Points the XDG homes at
// the build's isolated-home before main() runs, so a test binary started by
// hand, not through ctest, still never touches the real ~/.config, ~/.cache or
// ~/.local/share - a direct run once left thousands of history repos there.
#include <cstdlib>
#include <string>

namespace {
const bool isolatedHome = [] {
    const std::string home = NOTEAPPC_TEST_HOME;
    setenv("XDG_CONFIG_HOME", (home + "/config").c_str(), 1);
    setenv("XDG_CACHE_HOME", (home + "/cache").c_str(), 1);
    setenv("XDG_DATA_HOME", (home + "/data").c_str(), 1);
    return true;
}();
}
