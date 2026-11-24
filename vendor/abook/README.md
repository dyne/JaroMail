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
private build as `0.6.2+jaromail.1`, following the existing upstream version line.
The native format header still uses the upstream version. Manuals, the sample
configuration and the original `COPYING` and `AUTHORS` notices ship with the
private executable. User/system prefixes and `DESTDIR` follow the main project.

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

## Reproduce and update the fork

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
