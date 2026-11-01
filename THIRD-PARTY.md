# Third-party components

Notolith itself is GPL-3.0-or-later (see `LICENSE`). This file lists what else
goes into a build of it, with the licence of each. Everything under "Fetched at
configure time" is downloaded by CMake at the pinned commit or tag named in the
CMake file given; none of it is copied into this repository.

## Fetched at configure time (default build)

| component | licence | pinned in | used for |
|---|---|---|---|
| md4qt 5.1.4 (Igor Mironchik) | MIT | `CMakeLists.txt` | Markdown parser; patched at configure time by `cmake/patch-md4qt.sh` |
| MicroTeX (Nano Michael), commit 0e3707f | MIT | `CMakeLists.txt` | LaTeX maths rendering; patched at configure time by `cmake/patch-microtex.sh` |
| MicroTeX fonts, compiled into the binary | Knuth licence, dsrom licence, SIL OFL 1.1 | MicroTeX `res/fonts` | maths glyphs |
| tinyxml2 10.0.0 (Lee Thomason) | zlib | `CMakeLists.txt` | MicroTeX's XML reader |
| yaml-cpp 0.8.0 (Jesse Beder) | MIT | `CMakeLists.txt` | only when no system yaml-cpp is found |

## In this repository

| component | licence | where |
|---|---|---|
| Lucide icons | ISC | `libs/theme/icons/` (notice in `libs/theme/icons/LICENSE`; the app icon `noteAppC.svg` is the project's own, GPL-3.0-or-later) |
| Peer-to-peer link layer (the author's Hearth project) | GPL-3.0-or-later | `libs/ext/link/` |
| Demo vault assets | per file, see `docs/DEMO-ASSETS.md` | `demo/Embeds/assets/` |

## Optional builds

Built only with their CMake options (`docs/OFFLINE.md`).

- Note sharing with the yrs backend (`NOTEAPPC_WITH_SHARE_YRS`) builds the Rust
  crate in `libs/ext/share/yrs-ffi`, which pulls in yrs (MIT) and the crates in
  its `Cargo.lock`. Their licences have not been audited yet.
- Note sharing (`NOTEAPPC_WITH_SHARE`) links libdatachannel (MPL-2.0),
  libsodium, avahi, protobuf and nlohmann_json from the system.

## System libraries

The runtime dependencies of the Arch packages (`depends` in
`packaging/arch/PKGBUILD`). They are not part of this repository or of the
packages; pacman installs them, and each keeps its own licence. Licences are
as `pacman -Qi` reports them (checked 2026-11-01).

| library | licence |
|---|---|
| Qt 6 (`qt6-base`, `qt6-svg`, `qt6-multimedia`) | GPL-3.0-only, LGPL-3.0-only, Qt commercial licence, Qt GPL exception 1.0 |
| yaml-cpp | MIT |
| hunspell | LGPL-2.1-or-later OR GPL-2.0-or-later OR MPL-1.1 |
| KSyntaxHighlighting (`syntax-highlighting`) | MIT |
| litehtml | BSD-3-Clause |
| libgit2 | GPL-2.0-only with a linking exception |
| libarchive | BSD-2-Clause, BSD-4-Clause-UC, and Apache-2.0 OR CC0-1.0 OR OpenSSL |
| zeromq | MPL-2.0 |
| libseccomp | LGPL-2.1-only |
| bubblewrap | LGPL-2.1-or-later |
| util-linux (`prlimit`) | BSD-2-Clause, BSD-3-Clause, BSD-4-Clause-UC, GPL-2.0-only, GPL-2.0-or-later, GPL-3.0-or-later, ISC, LGPL-2.1-or-later, public domain (varies by tool) |
| poppler-qt6 (`notolith-pdfview`) | GPL-2.0-only, GPL-3.0-or-later, LGPL-2.0-or-later, LGPL-2.1-or-later, MIT |

Libraries that only an optional build links (libdatachannel, libsodium, avahi,
protobuf, nlohmann_json) are named under "Optional builds". All of them keep
their own licences.

## Licence texts

The MIT, ISC and zlib texts below are as shipped by each project. MicroTeX does
not say which licence covers which font file; the three texts are reproduced as
it ships them. The SIL OFL names `eufb10`, `eufm10`, `msam10` and `msbm10`.

### Lucide icons (ISC)

```text
ISC License

Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as part
of Feather (MIT). All other copyright (c) for Lucide are held by Lucide
Contributors 2022.

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
```

### MicroTeX (MIT)

```text
The MIT License (MIT)

 Copyright (c) 2020 Nano Michael

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
```

### md4qt (MIT)

```text
MIT License

Copyright (c) 2026 Igor Mironchik <igor.mironchik@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### yaml-cpp (MIT)

```text
Copyright (c) 2008-2015 Jesse Beder.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### tinyxml2 (zlib)

```text
This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
```

### MicroTeX fonts: Knuth licence

```text
Knuth License

This software is copyright and you are explicitly granted a license which gives you, the "user" of the software, legal permission to copy, distribute and/or modify the software, so long as if you modify the software then it carry a different name from the original software.

All the same, please check the specific details of the software's license before making modifications. 
```

### MicroTeX fonts: dsrom licence

```text
License
-------

You may use and distribute these fonts as you like.
You may modify these fonts as long as you do not
rename the files to one of those names that 
Donald E. Knuth chose for the Computer Modern fonts.
(And seriously, who would want to do that?)
```

### MicroTeX fonts: SIL Open Font License 1.1

```text
Copyright (c) 1997, 2009, American Mathematical Society (http://www.ams.org).
All Rights Reserved.

"eufb10" is a Reserved Font Name for this Font Software.
"eufm10" is a Reserved Font Name for this Font Software.
"msam10" is a Reserved Font Name for this Font Software.
"msbm10" is a Reserved Font Name for this Font Software.

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL

-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```
