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
cmake --install out/build --prefix "$HOME/.local"
```

For package staging, set `DESTDIR` when running `cmake --install`. Both staged
installations and extracted binary archives resolve their resource directories
relative to `bin/jaro`; build-machine paths are not needed to run them.

The old Make commands and platform entry points delegate to CMake. There is no
separate native compiler recipe in the compatibility Makefiles. Native test
callers enable `JARO_BUILD_TEST_DRIVERS`; ordinary installations and release
packages include only the seven application helpers.
In-source CMake configuration is refused to protect the compatibility Makefile.
The old optional bundled mutt-kz/Mixmaster build aliases are outside the supported
core build; use the external mail clients listed in the README.

Committed manpages are installed unchanged. Maintainers regenerate explicitly
with `cmake --build out/build --target man` and commit the generated `.1` files
alongside their Markdown sources. No wall-clock date is embedded.

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
revision and the checksum-verified Dyne musl v2.5.1 toolchains. Both builds run on
an x86_64 Linux host:

| Artifact architecture | Action target | Compiler tuple | Runtime smoke test |
| --- | --- | --- | --- |
| x86_64 | x86_64 | x86_64-linux-musl | Native |
| arm64 | arm_64 | aarch64-linux-musl | qemu-aarch64 |

Each of the seven helpers must be an ELF64 binary of the correct machine type,
with neither an ELF interpreter nor shared-library dependencies. Failed static
checks, tests, smoke tests or either architecture prevent publication.

CPack creates `jaromail-VERSION-linux-ARCH.tar.gz`; each archive has a matching
`.sha256` file. Archives contain `bin/jaro`, the native helpers, Zsh modules,
version metadata, committed manuals, configuration templates and license text.
The scripts still need Zsh and the normal external mail tools, including
fetchmail, msmtp, neomutt, notmuch, pinentry, abook, wipe and mblaze. These tools
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
