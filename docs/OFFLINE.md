# The default build is offline

The default build has no networking and no encryption. Nothing in it opens an
internet socket, and no crypto library is compiled or linked. Each feature that
does is a CMake option, `OFF` by default:

| option | what it adds | needs |
|---|---|---|
| `NOTEAPPC_WITH_NETWORK` | `reference` blocks that fetch a URL, remote icons, remote images, word lookup from a DICT server, and the network permission of a plugin | Qt6::Network, bubblewrap at run time for images |
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
- Remote images in notes (`![](https://...)`, `<img src="https://...">`) are fetched only with
  `NOTEAPPC_WITH_NETWORK=ON`, and only when asked (next section). The default build has no code for
  them and shows the text.
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

## Remote images

Only a `NOTEAPPC_WITH_NETWORK=ON` build. Nothing in a vault makes a request or decodes an image on its own:

- Opening a note draws a small placeholder for each remote image: the host and a "Load image" button.
  A click loads that image for as long as the note is open. "Always load from <host>" also remembers the
  origin (`https://host[:port]`), and images from it load without a click. The list is the file
  `remote-image-hosts` in the application's state directory (`$XDG_STATE_HOME/notolith`), never in a vault.
  Settings > Editor > "Remote images" > "Clear list" empties it. The row "Fetch remote content" does not
  govern images: the click does.
- Fetch: https only (http shows a notice). No `user:pass` in the URL. No `localhost`, `*.localhost`, and no
  host that is, or resolves to, a loopback, private, link-local, shared (CGNAT) or unique-local address.
  At most 3 redirects, each checked by the same rules and never to http. No cookies, no credentials, no
  `Referer`, `User-Agent: Notolith`, no compression. At most 20 MiB (stopped on Content-Length or while
  reading) and 30 s in all. DNS rebinding is not covered: a name is resolved for the check and again for the
  request, as for `reference` blocks.
- What the bytes are is told by their first bytes: PNG, JPEG, GIF or WebP, and nothing else (SVG, BMP, TIFF
  and the rest are refused, whatever the Content-Type or the file name says).
- Decode: out of process. `notolith-imagedecode` (installed in `<libexec>/notolith`) reads the bytes on
  stdin and writes a 20-byte header (width, height, format) and raw pixels on stdout. The app starts it
  under bubblewrap with no network, no home, the seccomp filter and CPU, memory and file-size limits
  (`libs/sandbox`). Inside it, `QImageReader` has the allocation limit at 64 MiB and the pixel count from
  the header is capped at 16 megapixels before any decoding. The app checks the reply (magic, status,
  dimensions, the format it sniffed itself, exactly 4 bytes per pixel) before it makes a `QImage`. A GIF
  shows its first frame. Without bubblewrap, or without the helper, nothing is downloaded or decoded and
  the placeholder says so. One helper process per image: a hostile image never meets another one.
- Cache: the original bytes in `$XDG_CACHE_HOME/notolith/remote-images/` (directory 0700, files 0600),
  named by the SHA-256 of the URL, at most 100 MiB in all, the oldest removed first. They have no expiry,
  and cached bytes go through the helper like fresh ones. A cached image still needs the click.
- An image in a table cell stays source text.

Measured on starbase-1 (median of 9, the helper under bubblewrap, at idle priority on 12 cores):

| image | in-process decode | helper, new process | helper, kept running |
|---|---|---|---|
| 1 MP PNG | 11 ms | 27-37 ms | 14-18 ms |
| 4 MP JPEG | 10 ms | 40-58 ms | 27-35 ms |

Starting the sandbox and the helper with nothing to decode takes 9-13 ms; the rest is the decode and the
pixels (4 MiB or 16 MiB) crossing a pipe and being copied. A helper kept running saves 12-24 ms an image and
was left out: it would hold every image from every site in one process. Neither fits a 10 ms budget, and a
local image does not go through the helper, so the budget does not apply; for a remote one the network is
the cost. The numbers come from `ImageDecoderTest.DISABLED_Latency` in `tests/test_remote_images.cpp`.
