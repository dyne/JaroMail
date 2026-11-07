# Vendored abook 0.6.2

`upstream/` contains **all 126 regular files** from the complete release archive:

- Source: https://abook.sourceforge.io/devel/abook-0.6.2.tar.gz
- Archive SHA-256: `2d6bde2d2d03523f164f930e4fdec6025f3a94abe48a43706f543880a1a21ebe`
- License: upstream `COPYING` (GNU GPL version 3), with attribution in `AUTHORS`.

No files were omitted or changed at import: C sources/headers, manuals, examples,
translations, documentation and the upstream Autotools files are all retained.
The tarball itself is not committed. `sources.json` records every file's size and
SHA-256. `python3 vendor/verify.py` verifies both this tree and the separately
scoped minimal Sieve extraction without network access.

## CMake integration

[`cmake/abook/`](../../cmake/abook/) compiles the current source tree directly as
the default `abook` target; no Autotools or source-tree generation is needed.
Build just abook with `cmake --build out/build --target abook`. Generated headers
and optional message catalogs stay in the build directory. The CMake header
supplies the missing `<ctype.h>` declaration used by upstream `database.c`
and `<string.h>` declarations for bundled getopt on musl, and
the target retains upstream's GNU inline semantics for modern C compilers.

Native builds require wide-character ncurses and **GNU** readline development
libraries (`libncurses-dev libreadline-dev` on Debian/Ubuntu). UTF-8 handling is
enabled when the target libc provides the required multibyte functions. Optional
`-DJARO_ABOOK_NLS=ON` builds the five upstream message catalogs with gettext;
the default build uses English messages and needs no gettext runtime. The
upstream optional libvformat integration remains disabled, as in its default
configure; the built-in vCard importer remains available.

Install puts abook in `share/jaromail/bin` alongside Jaromail's helpers, and
Jaromail's existing runtime PATH selects it first. It does not replace a system
`bin/abook`. Manuals, sample configuration and copyright/license notices are
installed too. User/system prefixes and `DESTDIR` follow the main project.

Linux release builds use target ncurses/readline archives prepared by
`build/abook-deps.py`; their sources are fetched only in that explicit build
step, never by ordinary CMake configure/build. The source versions, URLs and
SHA-256 pins are in `build/abook-deps.json`. Dependencies are built outside the
vendor tree, and their notices accompany static packages.

## Customization

This import is a clean baseline for local changes. Edit `upstream/` sources in
normal reviewable commits, add a regression in `extras/test/cmake_release_test.py`
for changed import/export behavior, and document the change here. Update the
corresponding file digests/sizes in `sources.json` for deliberate changes; retain
the original release URL/archive hash so upstream differences remain traceable.
Build wiring belongs in `cmake/abook/`, rather than editing generated upstream
Makefiles. No addressbook behavior has been changed in this initial import.
