#!/bin/sh
# Builds noteAppC-<arch>.AppImage from this working tree with linuxdeploy and
# its Qt plugin. Not yet run: it downloads both tools from
# github.com/linuxdeploy, and linuxdeploy insists on an application icon,
# which noteAppC does not have yet (packaging/noteAppC.svg, see TODO.md).
#
#   sh packaging/appimage/build-appimage.sh
#
# Everything happens under /tmp, because ~/Documents is synced. CFLAGS and
# CXXFLAGS are dropped so a distribution's -march=native cannot leak into an
# image meant for other machines.
set -eu

src=$(cd "$(dirname "$0")/../.." && pwd)
icon="$src/packaging/noteAppC.svg"
if [ ! -f "$icon" ]; then
    echo "no $icon: linuxdeploy needs an application icon" >&2
    exit 1
fi

work=/tmp/noteAppC-appimage
arch=$(uname -m)
mkdir -p "$work"
cd "$work"
for tool in linuxdeploy linuxdeploy-plugin-qt; do
    if [ ! -x "$tool-$arch.AppImage" ]; then
        curl -fLO "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-$arch.AppImage"
        chmod +x "$tool-$arch.AppImage"
    fi
done

env -u CFLAGS -u CXXFLAGS cmake -S "$src" -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build build
rm -rf AppDir
DESTDIR="$work/AppDir" cmake --install build

# The SQLite driver and the SVG image plugin are loaded at run time, so the
# plugin's dependency scan cannot see them.
QMAKE=$(command -v qmake6 || command -v qmake) \
EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so" \
EXTRA_QT_PLUGINS="sqldrivers;imageformats;iconengines" \
    "./linuxdeploy-$arch.AppImage" --appdir AppDir --plugin qt --output appimage \
    --desktop-file AppDir/usr/share/applications/noteAppC.desktop --icon-file "$icon"
ls -la "$work"/*.AppImage
