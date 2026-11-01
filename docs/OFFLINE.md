# The default build is offline

The default build has no networking and no encryption. Nothing in it opens an
internet socket, and no crypto library is compiled or linked. Each feature that
does is a CMake option, `OFF` by default:

| option | what it adds | needs |
|---|---|---|
| `NOTEAPPC_WITH_NETWORK` | `reference` blocks that fetch a URL, remote icons, word lookup from a DICT server, and the network permission of a plugin | Qt6::Network |
| `NOTEAPPC_WITH_AI_PLUGIN` | `plugins/ai` (turns `NOTEAPPC_WITH_NETWORK` on) | libcurl |
| `NOTEAPPC_WITH_SYNC` | sync to a server (`SYNC.md`) | libsodium, Qt6Keychain |
| `NOTEAPPC_WITH_ENCRYPTION` | encrypted notes, `.mdlock` (`ENCRYPTION.md`) | libsodium |
| `NOTEAPPC_WITH_SHARE`, `NOTEAPPC_WITH_SHARE_YRS` | note sharing (`SHARE.md`) | libdatachannel, avahi, protobuf, nlohmann-json, libsodium, cargo for the yrs backend |

An option that is off compiles none of its code and shows none of its menu
entries, commands or settings rows. A `.mdlock` file opened in such a build
says "This build of Notolith cannot open encrypted notes", and a `reference`
block with a URL says "Remote files are not supported in this build."

What remains, and why it is not the app talking to a network:

- Qt6::Network is linked for `QLocalSocket`, the single-instance socket (a Unix
  socket in the runtime directory). Dropping that link would not unmap it:
  Qt6Multimedia and KSyntaxHighlighting load libQt6Network themselves. The
  library is in the process; nothing calls a network class from it, which
  `offline_exe` and the socket trap below check.
- The settings file still knows the keys of the removed features
  (`sync.serverURL`, `sync.email`, `lookup.server`, `editor.remoteReferences`,
  `encryption.*` and the `sync`, `share`, `encrypted-notes`, `word-lookup`
  feature switches). A settings file from a networked build keeps them and
  they are written back; no code in this build acts on them and no settings
  page shows them. Keyring entries of an older build are not touched.
- The developer tools in the build tree (`noteAppC-bench`, `noteAppC_shot`)
  link the UI library directly and list libzmq and libQt6Network as NEEDED.
  They are not installed and `offline_exe` / `offline_modules` do not look at
  them.
- A link or "Search the web" opens the browser through `QDesktopServices` on a
  click. The app sends nothing.
- Notebook kernels talk over libzmq on `ipc://` sockets only, in a sandbox with
  its network namespace unshared.
- Remote images in notes (`![](https://...)`) are not fetched in any build:
  there is no fetching code for them.
- With `NOTEAPPC_WITH_NETWORK=ON`, every fetch that exists (a `reference`
  block with a URL, an icon given as a link) is behind one setting,
  `editor.remote-references` (Settings > Editor > "Fetch remote content"),
  which is off by default. Off, nothing is sent and the block or icon shows a
  notice or stays blank. Word lookup is separate: it sends a request only when
  the person asks for a lookup.

The ctest entries `offline_exe` and `offline_modules` (`tests/check_offline.sh`)
fail when the executable or a module lists a networking or crypto library as
NEEDED, or names a Qt network client class or a libc name-resolution or
datagram call. Only direct dependencies are visible to them: libgit2 (note
history), libarchive (EPUB) and Qt6Network themselves link libcrypto, so it is
in the process through them.

Link-time checks cannot see a library opened with `dlopen` or a socket made by
code that is loaded later, so every test of the default build also runs under
`tests/nettrap.c`, an `LD_PRELOAD` library that ends the process (exit status
86) on `socket()` with `AF_INET` or `AF_INET6`, on `connect()` to anything but
a Unix socket, on `getaddrinfo`, `gethostbyname*` and `getnameinfo` of an inet
address, and on `dlopen` of a network library. Unix sockets (the
single-instance socket, notebook `ipc://`) pass. The trap applies to the test
process only, not to programs it starts (a Jupyter kernel binds its own
loopback ports). The `nettrap_*` entries run a probe that makes each call, so
the trap itself is tested.
