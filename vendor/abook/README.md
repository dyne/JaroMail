# JaroMail's abook fork

`upstream/` started with **all 126 regular files** from the complete release archive:

- Source: https://abook.sourceforge.io/devel/abook-0.6.2.tar.gz
- Archive SHA-256: `2d6bde2d2d03523f164f930e4fdec6025f3a94abe48a43706f543880a1a21ebe`
- License: upstream `COPYING` (GNU GPL version 3), with attribution in `AUTHORS`.

The upstream Git tag and archive hash identify the release. The `files` array
in `sources.json` retains every original size and SHA-256, including the generated
Autotools files and notices. The `fork` section records the reviewed upstream
tip, adopted commits, private version and current hashes for changed files.
`python3 vendor/verify.py` uses the pristine inventory with those changes applied
to check the working tree offline. Keep a copy of the archive for reproduction.

## CMake integration

[`cmake/abook/`](../../cmake/abook/) compiles the fork directly as
the default `abook` target; no Autotools or source-tree generation is needed.
Build just abook with `cmake --build out/build --target abook`. Generated headers
and optional message catalogs stay in the build directory. CMake supplies
`<string.h>` declarations for bundled getopt on musl, and
the target retains upstream's GNU inline semantics for modern C compilers.

Native builds require wide-character ncurses and **GNU** readline development
libraries (`libncurses-dev libreadline-dev` on Debian/Ubuntu). UTF-8 handling is
enabled when the target libc provides the required multibyte functions. Optional
`-DJARO_ABOOK_NLS=ON` builds the five upstream message catalogs with gettext;
the default build uses English messages and needs no gettext runtime. The
upstream optional libvformat integration remains disabled, as in its default
configure; the built-in vCard importer remains available.

Install puts abook in `share/jaromail/bin` alongside Jaromail's helpers, and
Jaromail's existing runtime PATH selects it first. `abook --help` identifies the
private build as `0.6.2+jaromail.4`, following the existing upstream version line.
The native format header still uses the upstream version. Manuals, the sample
configuration and the original `COPYING` and `AUTHORS` notices ship with the
private executable. User/system prefixes and `DESTDIR` follow the main project.

Mutt queries use canonical Unicode decomposition, case folding and alphanumeric
search keys for names, individual mailboxes, nicknames and notes. Queries leave
contact text unchanged. The Unicode 15.1.0 data used by the matcher is committed
under `cmake/abook/`, with its license and a generator for maintainers. Builds
use the committed data and require no Unicode library or generation step.

`jaro abook` opens the selected list in the profile's `contacts.sqlite`.
Press `o` to switch lists, `/` to filter as you type, and Esc to clear the
filter. Browse with the arrow keys, `j`/`k`, or Ctrl+N/Ctrl+P. The `>`
marker shows the active contact, and Space toggles its separate `[x]` mark.
Enter edits the contact, and Tab shows its mailboxes. Wider terminals also
show a contact preview. The list picker includes block, which recipient
completion excludes.
In the form, use arrows, Tab, or Ctrl+N/Ctrl+P to focus a field and Enter
to edit it. The number and letter shortcuts still edit fields directly.
Use Left/Right, `h`/`l`, or Ctrl+B/Ctrl+F to switch field groups, and
`j`/`k` to move to the next or previous contact.
The editor uses the terminal theme's foreground and background on
color-capable terminals. Reverse video distinguishes focus, and the cursor
and mark symbols are visible on monochrome terminals too.
Press `w` to save and `q` to return from the form. With autosave enabled,
leaving a draft saves it first. A failed save keeps the draft visible. Use
`Ctrl+R` in the form or `l` in the list to discard the draft and reload after
confirmation. The editor writes changed records and confirmed removals in
one transaction. A removal affects only the list shown on screen.

The private executable also retains its upstream file conversion and
standalone query commands.

The legacy migration reader accepts numeric contact sections in file order,
an optional `[format]` header, and empty books. It preserves field keys and
values, including unknown fields, empty values, tabs, and equals signs in
values. It accepts LF, CRLF, and a final line without a terminator. Duplicate
field keys, missing names, and malformed sections produce line, record, and
field diagnostics without displaying contact values. Limits are 64 KiB per
physical line excluding LF, 128 fields per contact, and 64 MiB per source
including line endings.

Linux releases are fully static, including abook. Dyne musl **v2.6.0** supplies
both ncurses and GNU readline beside the compiler at `/opt/dyne/<target-triplet>`.
`build/abook-deps.py --require-bundled` stages those target archives and headers;
the release pipeline uses this strict mode instead of building dependencies.
Library/header hashes and versions enter the cache identity and package provenance.
The executable contains musl, ncurses and readline; host ncurses/readline packages
are not runtime dependencies.

For manually selected older musl toolchains, the explicit dependency command
retains pinned source fallbacks from `build/abook-deps.json`, available with
`--download` instead of strict mode. Ordinary CMake configure/build never downloads
dependencies. The normal release pipeline requires the complete prebuilt bundle.

## External contact actions

The mail and browser actions use `mutt_command` and `www_command` as a program
followed by optional arguments. Use spaces or tabs between words. Use single
or double quotes to group text, or use a backslash outside single quotes to
include the following character literally. The launcher passes these words
directly to the program and appends one argument containing the recipient list
or URL. The defaults are `mutt` and `lynx`. The JaroMail editor configures
`set mutt_command=jaro compose`.

An outer pair of double quotes encloses the whole value in `abookrc`. Use
single quotes around a program path containing spaces.

For example, `set www_command='/opt/Browser Tools/browser' --new-window` runs
that executable with `--new-window` and the contact URL as two arguments.
For settings that need expansion, pipelines or redirection, use a wrapper
program that accepts the appended argument. The browser action accepts absolute
HTTP and HTTPS URLs with an authority. Encode spaces in URL text as `%20`.

Generated Mutt and NeoMutt settings leave the `%s` placeholder unquoted in
the shell command so the client can quote the completion text itself.
[Mutt manual](https://www.mutt.org/doc/manual/#query-command)
[NeoMutt documentation](https://docs.neomutt.org/reference/config/alias.html#query-command).

## Reproduce and update the fork

### Qualification checks

Run the vendor verifier and the native strict suite with the prepared Sieve
and test dependencies described in the repository guidelines.

```sh
python3 vendor/verify.py
cmake -S . -B out/build -DCMAKE_BUILD_TYPE=Release "-DJARO_SIEVE_DEPS_PREFIX=$SIEVE_DEPS_PREFIX"
cmake --build out/build --target check-strict
```

The strict suite runs the relocated installed-consumer tests, including
first-use import, Mutt and vCard round trips, SQLite doctor, and the real
curses editor over a PTY. The native package smoke also runs in that suite.
For a packaged Linux build, unpack the archive into a different directory
and inspect the private binaries before running the same smoke:

```sh
python3 build/check-static.py --bin-dir "$prefix/share/jaromail/bin" --arch "$arch"
python3 extras/test/abook-package-smoke.py --prefix "$prefix" --version "$version" --cli
```

Use `--emulator qemu-aarch64` in place of `--cli` for arm64 helpers on an
x86_64 host. This checks the installed private abook and SQLite contact
helper. The full shell check runs on a host that can launch the package's
helpers directly. The smoke reports the selected execution path, checks,
and executable hashes.

Update the synthetic fixtures alongside the specific behavior change. The
[query fixtures](../../extras/test/fixtures/abook-query/README.md) document
approved query-protocol deltas from the pristine release. The conversion
tests compare parsed records and preserve mailbox spelling and order. The
built-in vCard converter drops unmapped properties and Mutt alias groups.
The Mutt alias export splits a multi-address contact and drops its note.
The package fixture also checks the previously dropped final alias without
a newline. Review changes in the corresponding expected records when a
converter mapping changes.

### Replay the downstream delta

From a clean checkout of the desired JaroMail revision, replay its committed
source diff against a local copy of the pinned archive. Commit
`fe8d479adc0a970f8e41676c8923e89dd4a857c7` is the pristine vendor import for
0.6.2. Set `archive` to the absolute path of the tarball.

```sh
archive=/path/to/abook-0.6.2.tar.gz
pristine=fe8d479adc0a970f8e41676c8923e89dd4a857c7
replay=$(mktemp -d)
mkdir -p "$replay/vendor/abook/upstream"
printf '%s  %s\n' \
  2d6bde2d2d03523f164f930e4fdec6025f3a94abe48a43706f543880a1a21ebe "$archive" | sha256sum -c -
tar -xzf "$archive" -C "$replay/vendor/abook/upstream"
git diff --binary "$pristine" HEAD -- vendor/abook/upstream > "$replay/abook.patch"
git -C "$replay" apply --check "$replay/abook.patch"
git -C "$replay" apply "$replay/abook.patch"
diff -r vendor/abook/upstream "$replay/vendor/abook/upstream"
python3 vendor/verify.py
```

Stop at a failed check. Review the patch alongside the commit dispositions
below. The Git diff includes the vCard ownership adjustment and the private
help text in addition to the adopted upstream changes.

For a later upstream release, verify its archive digest and compare every
extracted file with its Git tag, including generated files. Record the pristine
import before replaying the reviewed JaroMail changes. For a post-tag update to
the same release, apply selected changes to a disposable copy of the current
tree and review the generated source diff against that tree. For either update,
test the query, consumer and format fixtures before accepting the diff. Refresh
the pristine file identities only when changing the release archive. Update
`fork.modified_files`, the commit review and the private revision to describe
the shipped changes. Keep build wiring in `cmake/abook/` and preserve the
release's notice files.

## Upstream delta review

All 126 regular files in the pinned archive match the corresponding blobs at
`ver_0_6_2` (`a243d4a18a64f4ee188191a797b34f60d4ff852f`) byte for byte.
The tag has one additional file, `.gitignore`. The archive includes generated
`configure`, `aclocal.m4`, `config.h.in`, `Makefile.in`, `po/Makefile.in.in`
and `stamp-h.in`, plus the Autotools helper scripts. The archive SHA-256
identifies the compressed release independently of the Git tree.

The eight commits from the release tag through the reviewed tip
`b5e4a4daec1c7842e214b50a0f267e07d9792311` were assessed with JaroMail's CMake
build of the archive, a trial build with the full eight-commit delta, and the
selected changes. All three passed the byte-exact query fixtures and the
installed JaroMail, NeoMutt and filter snapshot fixtures.

| Upstream commit | Reachability, evidence and disposition |
| --- | --- |
| `b794aa1ad3cd9e74d6534010ccc5766aef4562ca` | **Defer symlink following.** The change applies to legacy editor saves and `--add-email-quiet`. With a link to an existing book, the trial kept the link and moved the original referent to a backup beside it. The archive and fork replace the link itself and back up the link beside the book. The trial crashed creating a fresh book or saving through a dangling link because `realpath()` returned no path; both actions succeeded with the archive and fork. The patch also retains the two-rename save sequence. |
| `579b24b0673eb1b837b24b6206690bc5c1ab8b81` | **Defer the `field_id()` header change.** This affects compiler declarations for callers of the field lookup. The archive, trial and fork linked with JaroMail's existing GNU inline mode. The commit leaves the definition inline, so the fork keeps the current toolchain contract. |
| `7c85eaa724bf2ed29972a9ce5a0d9da566eff7cc` | **Defer the release-build rewrite policy.** The trial kept both bytes and comments and created no backup when an untouched book received `w` and normal quit. The archive and fork rewrote it and removed the comment. The guard also affects autosave on file switch, and forced saves bypass it. Keep legacy behavior until the editor can verify all dirty transitions and handle save failures. |
| `c8ad7c59603c97c1b60009ff7ff1489258259b37` | **Defer early nickname declaration.** Default Mutt queries already match nicknames, as confirmed by the release fixture. Always declaring the field also changes behavior for configurations that omit it with `preserve_fields=none`. Search policy belongs with the completion changes. |
| `39484721c44629fb1f54d92f09c92ef4c3201302` | **Adopt the compiler cleanup.** The three exporter interfaces take NUL-terminated strings, including short literals from query, conversion and print paths. Using pointer declarations removed four GCC diagnostics about an apparent eight-byte buffer requirement. Adding the direct `ctype.h` include and dropping the unused variable are also part of this commit. |
| `887082c9912b245c977d2791b2997a4ac568c64f` | **Defer the `allcsv` header change.** The converter and editor can export this format. A CSV reader saw `name` in the trial and `#"name"` in the archive and fork. Preserve existing exported bytes until the format contract is reviewed. |
| `d3adcdb503619d62b18e3edd7845ca560e32b738` | **Defer Autotools libvformat link ordering.** This changes an optional Autotools link path. The JaroMail CMake build uses the built-in importer and never consumes that Makefile or links libvformat. |
| `b5e4a4daec1c7842e214b50a0f267e07d9792311` | **Adopt the built-in vCard field mapping with an ownership adjustment.** Direct array indexing misfiled nickname, URL, note and birthday when views declared fields in a different order. The regression exercises vCard → abook → vCard → abook with reordered fields and two mailboxes. The fork transfers the parser's existing allocation, frees replaced values and frees values rejected by the field lookup. Unsupported vCard properties are ignored. A separate two-save native-book fixture confirms unknown fields are retained with `preserve_fields=all`. |
