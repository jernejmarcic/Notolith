#!/bin/sh
# Builds a release of this working tree in /tmp and installs it for this user
# only: ~/.local/bin/notolith, its desktop entry (the app menu) and its icon.
# Run it again to update. No sudo, no package; packaging/arch for that.
#
# The desktop entry goes in without its MimeType line: with it, the mime cache
# lists Notolith first and it silently becomes the default for .md files and
# folders. NOTOLITH_MIME=1 keeps the line, for when that is wanted.
set -eu
src=$(cd "$(dirname "$0")/.." && pwd)
build=/tmp/notolith-release
data=${XDG_DATA_HOME:-$HOME/.local/share}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

env -u CFLAGS -u CXXFLAGS cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/dev/null
cmake --build "$build"
cmake --install "$build" --prefix "$stage" >/dev/null

install -Dm755 "$stage/bin/notolith" "$HOME/.local/bin/notolith"
desktop=io.github.jernejmarcic.Notolith.desktop
mkdir -p "$data/applications"
if [ "${NOTOLITH_MIME:-0}" = 1 ]; then
    cp "$stage/share/applications/$desktop" "$data/applications/$desktop"
else
    sed '/^MimeType=/d' "$stage/share/applications/$desktop" > "$data/applications/$desktop"
fi
install -Dm644 "$stage/share/icons/hicolor/scalable/apps/io.github.jernejmarcic.Notolith.svg" \
    "$data/icons/hicolor/scalable/apps/io.github.jernejmarcic.Notolith.svg"
update-desktop-database "$data/applications" 2>/dev/null || true
echo "installed $HOME/.local/bin/notolith"
