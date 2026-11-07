# Versions, CMake builds and Linux releases

The next release starts the **v6.0.0** series. The native filter engine and CMake
build migration are breaking changes from the last tagged v5 release.

## Build and install

Host build dependencies are CMake 3.20+, Python 3.9+, a C compiler, and Ninja or
Make. Thread and iconv support come from the platform's C library on Linux.
Optional ccache is enabled for compilation when found. Normal builds do not
download anything, invoke Pandoc, or need the private Sieve test libraries.

```sh
cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/build --parallel
cmake --install out/build
```

The default install prefix is `$HOME/.local`; `make install` uses the same
user-only default. Add `~/.local/bin` to `PATH` if necessary. The command,
resources and manuals go into `bin`, `share/jaromail` and `share/man/man1`
under that prefix. No `sudo` is needed.

`install-system` explicitly installs under `/usr/local`:

```sh
sudo make install-system
# Native CMake equivalent, after building:
sudo cmake --build out/build --target install-system
```

Both Make install targets honor `PREFIX` from the environment or command line
and `DESTDIR` for package staging. The native CMake `install-system` target
also honors the `PREFIX` environment variable and leaves the configured user
prefix intact. For example, Debian and other distribution packagers can run:

```sh
make install-system PREFIX=/usr DESTDIR="$PWD/package-root"
# Or use the already configured native CMake build:
PREFIX=/usr DESTDIR="$PWD/package-root" cmake --build out/build --target install-system
```

These commands stage `package-root/usr/bin/jaro`,
`package-root/usr/share/jaromail` and `package-root/usr/share/man/man1`
without writing to the live system.

For normal CMake installs, `-DCMAKE_INSTALL_PREFIX=/your/prefix` at configure
time or `cmake --install out/build --prefix /your/prefix` overrides the default.
A fresh configure also accepts `PREFIX` from the environment; an explicit
`CMAKE_INSTALL_PREFIX` takes precedence. Existing build directories retain their
cached prefix until reconfigured explicitly. Set `DESTDIR` when running
`cmake --install` to stage a normal install too. Both staged
installations and extracted binary archives resolve their resource directories
relative to `bin/jaro`; build-machine paths are not needed to run them.

The old Make commands and platform entry points delegate to CMake. There is no
separate native compiler recipe in the compatibility Makefiles. Native test
callers enable `JARO_BUILD_TEST_DRIVERS`; ordinary installations and release
packages include the seven application helpers and bundled abook 0.6.2.
In-source CMake configuration is refused to protect the compatibility Makefile.
The old optional bundled mutt-kz/Mixmaster build aliases are outside the supported
core build; use the external mail clients listed in the README.

Committed manpages are installed unchanged. Maintainers regenerate explicitly
with `cmake --build out/build --target man` and commit the generated `.1` files
alongside their Markdown sources. No wall-clock date is embedded.

## Bundled addressbook

CMake builds the full vendored abook 0.6.2 source alongside the native helpers.
Native builds need wide-character ncurses and GNU readline development libraries
(`libncurses-dev libreadline-dev` on Debian/Ubuntu). CMake generates its config
header outside the source tree and requires no Autotools or network access.
`cmake --build out/build --target abook` builds just the addressbook.

The binary is installed as `share/jaromail/bin/abook`, where the runtime already
searches first. System abook installations are untouched. Its two manual pages,
sample configuration, authors and GPLv3 license are installed with Jaromail.
`-DJARO_ABOOK_NLS=ON` optionally builds the five upstream translations with
gettext tools and libintl; the default uses English messages.

Static releases use **Dyne musl v2.6.0** and reuse its bundled target ncurses 6.6
and GNU readline 8.3 from `/opt/dyne/<target-triplet>`. abook, musl, ncurses and
readline are linked into one fully static executable. The explicit dependency
step uses `--require-bundled`: incomplete toolchains fail before any dependency
build or download. Both bundled library/header sets enter the cache identity and
installed provenance.

For manual builds with older musl toolchains, `build/abook-deps.json` retains
checksum-pinned source fallbacks. Release CI uses the prebuilt bundle directly.
`build/abook-deps.py` verifies cached installed bytes, stages target dependencies
in a private cache, and prints their prefix
for `-DJARO_ABOOK_DEPS_PREFIX=...`. Ordinary CMake builds never download them.
Both architectures run abook conversion/query smoke tests and open/quit its TUI
in a private pseudo-terminal (arm64 via QEMU). Embedded descriptions for common
terminals let the static TUI run even without a system terminfo database.
Dependency license notices and source pins accompany static packages.

See [source provenance and customization](../vendor/abook/README.md). The full
upstream source remains unchanged by this import; new behavioral changes belong
in later commits with focused addressbook regressions.

## Version assignment

`VERSION` contains the v6 development baseline. A source checkout uses its
highest reachable stable `vMAJOR.MINOR.PATCH` tag at or above that baseline.
Tags on unrelated branches and malformed or prerelease tags are not selected.
Git archive export metadata preserves the version when `.git` is unavailable;
unversioned source copies use the baseline. Fetch tags before building a checkout.
CMake tracks loose tag references and packed refs so incremental builds refresh
assigned metadata when tags change. `-DJARO_VERSION=6.2.3` explicitly assigns a
version, as the release workflow does before the new tag exists.

The assigned value is installed as `share/jaromail/VERSION`. The CLI reads it
for `jaro --version`, `jaro -v`, help, and the startup banner. Installed versions
do not contact GitHub or require Git/Python to report their version. The internal
filter compiler/cache ABI identifier is separate from the project's release
version and remains stable while that contract is unchanged.

## Automatic releases

Pushes to the default branch trigger `.github/workflows/release.yml`. The
workflow accepts both `master` (the current default) and `main`; publishing is
restricted to the repository's actual default branch. The workflow does not
rename branches or publish from pull requests. Manual dispatch on the default
branch can resume a failed release.

The version job uses
[`ietf-tools/semver-action`](https://github.com/ietf-tools/semver-action), pinned
to a reviewed revision. It compares the latest reachable stable tag against
the exact pushed commit. The first release injects a one-time major change,
validated to produce exactly `v6.0.0` from the existing v5 history. Thereafter,
Conventional Commits assign versions: `feat:` increments minor, `fix:`/`perf:`/
`refactor:`/`test:` increments patch, and `BREAKING CHANGE:` notes or breaking
headers increment major. Changes without a bump default to patch. Completed
release retries use the existing version rather than creating another tag.

Tests must pass before publishing. The Linux matrix uses
[`dyne/musl-action`](https://github.com/dyne/musl-action), pinned to a reviewed
revision and the checksum-verified Dyne musl v2.6.0 toolchains. Both builds run on
an x86_64 Linux host:

| Artifact architecture | Action target | Compiler tuple | Runtime smoke test |
| --- | --- | --- | --- |
| x86_64 | x86_64 | x86_64-linux-musl | Native |
| arm64 | arm_64 | aarch64-linux-musl | qemu-aarch64 |

Each of the eight executables (the seven helpers plus abook) must be an ELF64 binary of the correct machine type,
with neither an ELF interpreter nor shared-library dependencies. Failed static
checks, tests, smoke tests or either architecture prevent publication.

CPack creates `jaromail-VERSION-linux-ARCH.tar.gz`; each archive has a matching
`.sha256` file. Archives contain `bin/jaro`, the native helpers, Zsh modules,
version metadata, committed manuals, configuration templates and license text.
The scripts still need Zsh and the normal external mail tools, including
fetchmail, msmtp, neomutt, notmuch, pinentry, wipe and mblaze. These tools
are not bundled or rebuilt as part of this release.

Both architectures and checksums are uploaded to one draft GitHub release at
the triggering commit, then published together. Published releases are never
overwritten. A retry can complete an existing draft on the same commit; missing
or corrupt local artifacts and conflicting tag commits fail before publication.
Pull requests run the same static matrix and semantic-version preview as read-only
checks, uploading preview archives without creating tags or GitHub releases.

## Local verification

```sh
cmake --build out/build --target check-strict
ctest --test-dir out/build --output-on-failure
```

The full suite also needs the existing mail-tool fixtures and pinned private
Sieve development libraries described in the interface documentation. CI provisions
those separately; they do not enter the installed application or binary archives.
`build/check-static.py --bin-dir out/static/bin --arch x86_64` verifies a static
build made with `-DJARO_STATIC=ON`. Use a separate CMake build directory for each
architecture and toolchain.
