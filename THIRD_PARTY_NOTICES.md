# Third-party notices

Kobo Spark's original source, scripts, and documentation are licensed under GPL-3.0-or-later
(see [LICENSE](LICENSE)). Dependencies and bundled assets retain their own licences.

| Component | Source | Licence | Use |
| --- | --- | --- | --- |
| FBInk | [NiLuJe/FBInk](https://github.com/NiLuJe/FBInk) | GPL-3.0-or-later | E-ink rendering and Kobo device information |
| qimagescale (inside FBInk) | Qt Company / Daniel M. Duley / Carsten Haitzler and contributors | LGPL-3.0 or stated GPL option, with BSD-style notices | FBInk image/capture support; GPL-3.0 option used |
| libcurl | [curl.se](https://curl.se/docs/copyright.html) | curl licence (MIT-style) | HTTP/HTTPS client |
| Mbed TLS | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 or GPL-2.0-or-later; Apache-2.0 option used | TLS for libcurl |
| Atkinson Hyperlegible | [Google Fonts / Braille Institute](https://github.com/google/fonts/tree/main/ofl/atkinsonhyperlegible) | SIL Open Font License 1.1 | Bundled regular and bold fonts |
| Mozilla CA certificate data | [curl CA Extract](https://curl.se/docs/caextract.html) | MPL-2.0 | Public trust roots in `device/cacert.pem` |

The default executable statically links FBInk and is distributed under
GPL-3.0-or-later, matching the project's source licence. The build places the
licence at `dist/kobo-spark/LICENSE` and includes the project's copyright notice. Other
dependency licences and FBInk's credits are copied into the bundle. When distributing a binary,
provide its corresponding source and build scripts, including the pinned FBInk
and networking dependencies. `make source-bundle` packages these after a build.
FBInk's own headers and credits describe its additional third-party components.
The image-enabled build includes qimagescale; its source headers, copyright
notices (including Qt Company and Intel), and original image-transform licence
are included in the compiled bundle. Thanks to Daniel M. Duley, Carsten Haitzler,
and the Imlib2 contributors for the image-scaling implementation.

The font licence and provenance are checked in under `device/fonts/`. The Mozilla
MPL text is in `LICENSES/MPL-2.0.txt`. Compiler and library versions, source hashes,
and the immutable NickelTC compiler image are pinned in `toolchain.env`.

The dashboard is inspired by and consumes the API of
[MiaAI-Lab sparkDash](https://github.com/MiaAI-Lab/sparkDash). sparkDash runs
separately; its source is not included or linked into this client. Credit goes to
MiaAI-Lab and the sparkDash contributors for the monitoring backend and dashboard
that made this Kobo client possible.
