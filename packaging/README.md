# packaging

Notolith installs under the app id `io.github.jernejmarcic.Notolith`.

`io.github.jernejmarcic.Notolith.desktop` is what makes single-file mode
reachable: a file manager can open any `.md` with Notolith, and
`resolveLaunchTarget()` decides whether that means one file or the vault it
sits in. `inode/directory` is listed too, so "Open folder with" offers it for a
vault. `io.github.jernejmarcic.Notolith.metainfo.xml` describes the app to
software centres and Flatpak.

`cmake --install <build> --prefix /usr` puts the binary (`notolith`) in `bin/`,
the desktop file in `share/applications/` and the metainfo in
`share/metainfo/`.

Every recipe here builds the source tree it sits in. None of them passes
`-march`: a package has to run on any x86-64 or arm64 machine
(`docs/PLATFORMS.md`).

| recipe | state |
|---|---|
| `arch/PKGBUILD` | Builds with `makepkg -si --nocheck` in `arch/`; strips the `-march=native` that CachyOS puts in `makepkg.conf`. One build, five packages: `notolith` (the application), `notolith-ink` (handwriting core library and module, needs `abseil-cpp`), `notolith-hwfont` (glyph studio module, needs `notolith-ink`), `notolith-pdfview` (PDF viewer module, needs `poppler-qt6`) and `notolith-share` (note sharing). The module packages pin the exact `notolith` version, and all five of one run are installed together (`docs/INK.md`). |
| `flatpak/io.github.jernejmarcic.Notolith.yml` | Untested: needs flatpak-builder; the command is at the top of the file. PDF embeds are off because poppler is not in the KDE runtime. |
| `appimage/build-appimage.sh` | Untested: downloads linuxdeploy and its Qt plugin. |

The icon is `libs/theme/icons/noteAppC.svg`, installed to
`share/icons/hicolor/scalable/apps/` under the app id, and built into the
binary as the fallback window icon.
