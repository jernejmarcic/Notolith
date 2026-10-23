#!/usr/bin/env bash
# Builds Notolith from this (Syncthing-synced) working tree and installs it for
# the current user only, the way starbase-1 has it: ~/.local/bin/notolith plus
# the .desktop file and icon. No root except for the pacman line.
#
#   bash ~/Documents/AI/projects/noteAppC/packaging/install-local.sh
#
# Build and staging go to /tmp (never ~/Documents: Syncthing would replicate
# them). The first build fetches md4qt, MicroTeX and tinyxml2 (pinned in the
# root CMakeLists.txt), so it needs the network. Rerun to update.
# Remove: rm -r ~/.local/bin/notolith ~/.local/lib/noteAppC
#         ~/.local/share/applications/io.github.jernejmarcic.Notolith.desktop
#         ~/.local/share/icons/hicolor/scalable/apps/io.github.jernejmarcic.Notolith.svg
set -euo pipefail

src="$(cd "$(dirname "$0")/.." && pwd)"
build=/tmp/notolith-build
stage=/tmp/notolith-stage

# Runtime libraries (same set as starbase-1's build, incl. note sharing:
# libdatachannel + avahi) and build tools. --needed skips what is installed.
sudo pacman -S --needed \
    base-devel cmake ninja git pkgconf extra-cmake-modules gtest \
    qt6-base qt6-svg qt6-multimedia syntax-highlighting qtkeychain-qt6 \
    yaml-cpp hunspell hunspell-en_gb poppler-qt6 litehtml md4c libgit2 \
    libarchive curl libsodium zeromq libseccomp bubblewrap \
    libdatachannel nlohmann-json protobuf avahi

# Note sharing finds other devices over mDNS.
systemctl is-active --quiet avahi-daemon || sudo systemctl enable --now avahi-daemon

# No -march: the binary must run on any x86-64/arm64 (docs/PLATFORMS.md).
# CachyOS sets -march=native in makepkg.conf only, but strip it from the
# shell's flags too in case they carry it.
CFLAGS="$(printf '%s' "${CFLAGS:-}" | sed -E 's/-m(arch|tune)=[^ ]+//g')"
CXXFLAGS="$(printf '%s' "${CXXFLAGS:-}" | sed -E 's/-m(arch|tune)=[^ ]+//g')"
export CFLAGS CXXFLAGS

# The prefix is compiled in as the module directory (ink, handwriting font, PDF, sharing).
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"
# Capped so a runaway compile cannot take the machine down.
systemd-run --user --scope -q -p MemoryMax=8G nice cmake --build "$build"

rm -rf "$stage"
cmake --install "$build" --prefix "$stage"
# ECM's appstreamtest validates whatever the manifest lists; not needed here.
rm -f "$build/install_manifest.txt"

# install(1) unlinks the old file first, so a running Notolith keeps working.
install -Dm755 "$stage/bin/notolith" "$HOME/.local/bin/notolith"
# A module is only loaded by the build that made it: the old ones go.
modules="$HOME/.local/lib/noteAppC/modules"
mkdir -p "$modules"
rm -f "$modules"/*.so
for module in "$stage"/lib*/noteAppC/modules/*.so; do install -m755 "$module" "$modules/"; done
install -Dm644 "$stage/share/applications/io.github.jernejmarcic.Notolith.desktop" \
    "$HOME/.local/share/applications/io.github.jernejmarcic.Notolith.desktop"
install -Dm644 "$stage/share/icons/hicolor/scalable/apps/io.github.jernejmarcic.Notolith.svg" \
    "$HOME/.local/share/icons/hicolor/scalable/apps/io.github.jernejmarcic.Notolith.svg"
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

echo "Installed $(git -C "$src" rev-parse --short HEAD 2>/dev/null || echo '?') to ~/.local/bin/notolith"
case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) echo "Note: ~/.local/bin is not on PATH; start it from the app menu or add it." ;; esac
