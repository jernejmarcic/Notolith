# packaging

Notolith installs under the app id `io.github.jernejmarcic.Notolith`.

`io.github.jernejmarcic.Notolith.desktop` is what makes single-file mode
reachable: a file manager can open any `.md` with Notolith, and
`resolveLaunchTarget()` decides whether that means one file or the vault it
sits in. A vault is opened from inside the app or as `notolith <folder>`; the
desktop file does not register for every folder. `io.github.jernejmarcic.Notolith.metainfo.xml` describes the app to
software centres.

`cmake --install <build> --prefix /usr` puts the binary (`notolith`) in `bin/`,
the desktop file in `share/applications/`, the metainfo in `share/metainfo/`,
the licence and third-party notices in `share/licenses/notolith/`, the demo
asset notice in `share/doc/notolith/` and the plugin host (`notolith-plugin-host`)
in `<libexecdir>/notolith/`. The modules go to `<libdir>/notolith/modules/`.

Earlier Flatpak and AppImage recipes were untested and were removed; they are
in the git history. Notolith is installed with `cmake --install` from a source
build (see the README).

The default build has no networking or encryption dependency (`docs/OFFLINE.md`).
The PDF viewer is a separate module, `notolith-pdfview`, built when
`poppler-qt6` is found; a module is only loaded by the build that made it (the
ABI tag), so install the whole tree from one build.

The icon is `libs/theme/icons/noteAppC.svg`, installed to
`share/icons/hicolor/scalable/apps/` under the app id, and built into the
binary as the fallback window icon.
