# packaging

Notolith (code name noteAppC) installs under the app id `io.github.jernejmarcic.Notolith`.

`io.github.jernejmarcic.Notolith.desktop` is what makes single-file mode reachable: a file manager can
open any `.md` with Notolith, and `resolveLaunchTarget()` decides whether that
means one file or the vault it sits in. `inode/directory` is listed too, so
"Open folder with" offers it for a vault. `io.github.jernejmarcic.Notolith.metainfo.xml`
describes the app to software centres and Flatpak.

`cmake --install <build> --prefix /usr` puts the binary (`notolith`) in `bin/`, the desktop
file in `share/applications/` and the metainfo in `share/metainfo/`.

Every recipe here builds this working tree - there is no remote release to
fetch - and keeps its build directories in `/tmp`, because `~/Documents` is
synced to other machines. None of them passes `-march`: a package has to run on
any x86-64 or arm64 machine (`docs/PLATFORMS.md`).

| recipe | state |
|---|---|
| `arch/PKGBUILD` | built and checked here: `BUILDDIR=/tmp/noteAppC-makepkg PKGDEST=/tmp makepkg -f` in `arch/`. Strips the `-march=native` CachyOS puts in makepkg.conf. |
| `flatpak/io.github.jernejmarcic.Notolith.yml` | not yet built: needs flatpak-builder (deferred by Jernej 2026-10-16; the command is at the top of the file). PDF embeds off, poppler is not in the KDE runtime. |
| `appimage/build-appimage.sh` | not yet run: downloads linuxdeploy (deferred by Jernej 2026-10-16). |

The icon is `libs/theme/icons/noteAppC.svg` (hand-written SVG), installed to
`share/icons/hicolor/scalable/apps/` under the app id, and built into the
binary as the fallback window icon. The app id follows basket's
(`io.github.jernejmarcic.basket`); Jernej named the app Notolith on
2026-10-17.
