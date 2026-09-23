# packaging

`noteAppC.desktop` is what makes single-file mode reachable: a file manager can
open any `.md` with noteAppC, and `resolveLaunchTarget()` decides whether that
means one file or the vault it sits in. `inode/directory` is listed too, so
"Open folder with" offers it for a vault. `eu.mangoblue.noteAppC.metainfo.xml`
describes the app to software centres and Flatpak.

`cmake --install <build> --prefix /usr` puts the binary in `bin/`, the desktop
file in `share/applications/` and the metainfo in `share/metainfo/`.

Every recipe here builds this working tree - there is no remote release to
fetch - and keeps its build directories in `/tmp`, because `~/Documents` is
synced to other machines. None of them passes `-march`: a package has to run on
any x86-64 or arm64 machine (`docs/PLATFORMS.md`).

| recipe | state |
|---|---|
| `arch/PKGBUILD` | built and checked here: `BUILDDIR=/tmp/noteAppC-makepkg PKGDEST=/tmp makepkg -f` in `arch/`. Strips the `-march=native` CachyOS puts in makepkg.conf. |
| `flatpak/eu.mangoblue.noteAppC.yml` | not yet built: needs flatpak-builder (the command is at the top of the file). PDF embeds off, poppler is not in the KDE runtime. |
| `appimage/build-appimage.sh` | not yet run: downloads linuxdeploy, and needs an app icon first. |

Outstanding, all Jernej's to decide (see TODO.md): the app has no icon of its
own and borrows the freedesktop `accessories-text-editor` name; no licence has
been chosen; and the app id `eu.mangoblue.noteAppC` is a placeholder. With an
icon: put the SVG here as `noteAppC.svg`, set `Icon=noteAppC` in the desktop
file, and install it to `share/icons/hicolor/scalable/apps/`.
