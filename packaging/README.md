# packaging

Notolith installs under the app id `io.github.jernejmarcic.Notolith`.

`io.github.jernejmarcic.Notolith.desktop` is what makes single-file mode
reachable: a file manager can open any `.md` with Notolith, and
`resolveLaunchTarget()` decides whether that means one file or the vault it
sits in. `inode/directory` is listed too, so "Open folder with" offers it for a
vault. `io.github.jernejmarcic.Notolith.metainfo.xml` describes the app to
software centres.

`cmake --install <build> --prefix /usr` puts the binary (`notolith`) in `bin/`,
the desktop file in `share/applications/`, the metainfo in `share/metainfo/`,
the licence and third-party notices in `share/licenses/notolith/`, the demo
asset notice in `share/doc/notolith/` and the plugin host (`notolith-plugin-host`)
in `<libexecdir>/notolith/`. The modules go to `<libdir>/notolith/modules/`.

Arch (the PKGBUILD) is the supported package. It builds the tagged source and
its dependencies from git, each at a pinned commit and with no network in
`build()`, and passes no `-march` and no `target-cpu=native`: a package has to run
on any x86-64 machine (`docs/PLATFORMS.md`). Earlier Flatpak and AppImage
recipes were untested and were removed; they are in the git history.

`arch/PKGBUILD`: `makepkg -si` in `arch/` builds, runs the test suites (in a
second build directory with tests on) and installs; `--nocheck` skips the
tests. One build, two packages: `notolith` (the application) and
`notolith-pdfview` (PDF viewer module, needs `poppler-qt6`). The module
package pins the exact `notolith` version, and both of one run are
installed together. No package has a networking or encryption
dependency (`docs/OFFLINE.md`).

The icon is `libs/theme/icons/noteAppC.svg`, installed to
`share/icons/hicolor/scalable/apps/` under the app id, and built into the
binary as the fallback window icon.
