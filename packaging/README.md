# packaging

`noteAppC.desktop` is what makes single-file mode reachable: a file manager can
open any `.md` with noteAppC, and `resolveLaunchTarget()` decides whether that
means one file or the vault it sits in. `inode/directory` is listed too, so
"Open folder with" offers it for a vault.

`cmake --install build --prefix /usr` puts the binary in `bin/` and this file
in `share/applications/`.

Outstanding: the app has no icon of its own and borrows the freedesktop
`accessories-text-editor` name. Drop an SVG in here, point `Icon=` at
`noteAppC`, and add it to the install rules.
