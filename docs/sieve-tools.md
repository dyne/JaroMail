# Private Sieve validation tools

`make sieve-tools` builds only the extracted `sievec.c`, `sieve-test.c` and
`sieve-tool.c` against an explicitly supplied **Dovecot/Pigeonhole 2.4.5**
development installation. These are optional test oracles. Ordinary `make`,
`make install` and Jaromail do not build or load Dovecot.

```sh
SIEVE_DEPS_PREFIX=/absolute/private/development-prefix make sieve-tools
cat build/sieve-tools/current/build.json
```

The prefix must contain `include/dovecot/config.h`, `dovecot-version.h`, the
Dovecot public headers, `include/dovecot/sieve` including generated
`pigeonhole-config.h` and `pigeonhole-version.h`, and the shared development
libraries `lib/dovecot/libdovecot{,-storage,-lda,-sieve}.so`. Pigeonhole must
match revision `742bb8923b5aab398f8d962e827fcde7ad4a3f73`. A distribution's
2.4.1 libraries or system `sievec` binaries are not substitutes. No server
needs to be started. Use a disposable test configuration for evaluation;
never pass delivery/execution options against an actual mail root.

The offline builder checks header/library versions, revision and private
`mail_raw_*` exports, runs `vendor/verify.py`, and links the four libraries by
absolute prefix paths with an embedded private library search path. It never
downloads dependencies, starts a service or installs system files. `CC`,
`CPPFLAGS`, `CFLAGS`, `LDFLAGS` and `LDLIBS` are preserved as shell-style argument
lists. An available ccache wraps compilation only; `CCACHE=` disables it.
Set `CCACHE_DIR` to an owned cache when the default cache is unavailable.
Choose an owned absolute `SIEVE_TOOLS_OUTPUT` to change the default output.

Each build has a SHA256 identity covering upstream and build recipe bytes,
all installed development headers, the four linked library files, compiler
identity, ABI strings and flags. `current` selects a completed generation.
`current/build.json` records the exact compile/link commands, header dependency
digests, private symbols and output digests; `.d` files and linker maps remain
beside the binaries. Reuse rechecks dependencies and output bytes. Failed
compilation/linking leaves an inspectable `.building-*` directory and never
publishes that generation. Use a fresh output directory after corruption.
Only one builder may write an output directory at a time.
The upstream tools have no `--version` option: version/revision and ABI evidence
comes from the checked development headers and linked libraries in that record.

## Executing disposable oracle fixtures

```sh
SIEVE_DEPS_PREFIX=/absolute/private/development-prefix \
  zsh extras/test/test-sieve-oracles.sh
```

The oracle also needs that prefix's `bin/doveconf`,
`lib/dovecot/settings/libpigeonhole_settings.so` and
`lib/dovecot/libdovecot-managesieve.so`. These matching private configuration
components are independent of a running Dovecot service. The settings module
must identify Pigeonhole `2.4.5 (742bb892)`; its bytes, configuration, tools and
runtime dependencies are recorded with SHA256 digests.

`extras/test/sieve_oracle.py` exposes `SieveOracle.compile`, `evaluate` and
`expect` for later parity tests. It verifies/builds the selected source tools
before resolving their absolute private paths; it never resolves system Sieve
tools through PATH. Its minimal configuration uses owned home/mail/state/run
directories, explicit `doveconf` and settings module paths, and no user database.
It removes inherited Dovecot configuration descriptors/caches and does not pass
`-u` or delivery/execution options.

The harness copies every script/message into its private workspace. `sievec`
must produce a real binary; `sieve-test -C` compiles current source and simulates
actions with matching traces. The complete result report must contain exactly
one fileinto action with the expected literal mailbox and declared `:create`
side effect, and no implicit keep. Unknown output, extra actions and wrong
destinations fail. Source/copy bytes and empty private mail/home directories
are checked after simulation. The independent fixture corpus covers domain
apex/wildcard boundaries, deeper descendants, whole-mailbox ASCII case folding,
cross-address negatives, and mailbox creation without delivery.

Set `SIEVE_ORACLE_EVIDENCE` to a new absolute directory to retain per-invocation
commands, exits, stdout/stderr, binary outputs, interpreter traces, decisions
and `oracle.json`/`summary.json`; otherwise temporary evidence is removed.
Missing `SIEVE_DEPS_PREFIX` follows the local exit-77 skip contract. An explicitly
selected incomplete or mismatched prefix fails.

## Supplying source-built development prerequisites

The reproducible Linux CI provisioner needs Python 3.12+, GNU Make, a C
toolchain, Perl, pkg-config, binutils, GnuPG, OpenSSL and zlib development
packages; ccache is used when available. It installs no system files or service.
Run it with two owned directories outside the repository:

```sh
export SIEVE_DEPS_PREFIX="$(python3 build/sieve-deps.py \
  --cache-dir /absolute/external/sieve-deps \
  --source-cache /absolute/external/sieve-sources --download)"
make sieve-tools
make test-strict
```

`--download` authorizes acquisition of only the two official releases, their
detached signatures and the public signing key. Omit it for offline operation
with supplied prerequisites; `--key /absolute/public-key` selects a supplied
key. Every invocation verifies pinned archive hashes and the exact release
signer, including warm reuse. Compilation starts in an empty fingerprinted
generation with private source/build/install paths. `development.json` records
source/signature provenance, compiler/tool identities, configuration, flags,
all installed bytes/modes/symlink targets and exact build commands/log hashes.
`--verify-only` requires an already completed matching generation. Cache reuse
compares the complete prefix inventory; missing/extra/modified files or changed
modes fail. A failed/incomplete build never receives a reusable receipt; use a
fresh cache after failure or corruption. Use one provisioner per cache.

Caller `CC`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS` and configure `LIBS` enter the
dependency identity. Compilation uses the selected ccache launcher once;
`CCACHE=` disables it and link commands use the compiler directly. Set an owned
`CCACHE_DIR`, or the provisioner uses its own cache directory. Optional external
features are explicitly disabled by the recorded recipe; system toolchain and
OpenSSL/zlib identities qualify the cache. No full dependency source tree is
stored in Git.

The PR workflow caches external prerequisites and ccache, then always runs
the provisioner, `make sieve-tools` and `make test-strict`. A GitHub cache hit
does not bypass validation. Local `make test` reports missing oracle prerequisites
with exit 77. Strict execution fails on any such skip, wrong versions or missing
private tools. Normal `make` and `make install` remain independent of this target;
the application build/install regression exercises them without any Sieve prefix
and with unusable system Sieve tools.

### Optional regex oracle prerequisite

The minimal default dependency recipe disables PCRE2, so its regex extension
reports missing regular expression support. To investigate and qualify regex
exports, pass `--regex-prefix /absolute/private/pcre2-prefix` to
`build/sieve-deps.py`. That prefix must contain `include/pcre2.h` and one
`libpcre2-32.so` development link under `lib` or an architecture subdirectory,
resolving inside the same prefix. No system installation is performed.

The option uses the same signed and pinned Dovecot/Pigeonhole archives, enables
`--with-pcre2`, and records PCRE2 header/library hashes, link flags and runtime
search path in the build identity. The matched source-built frontends remain
test oracles. PCRE2 support alone does not qualify POSIX ERE equivalence: an
explicit supported subset and compiler-bound independent evidence are still
required before a regex export profile can be admitted.

### Manual source-build equivalent

Keep source archives, complete dependency trees and their build/install output
in an owned cache **outside this repository**. Do not expand `vendor/`.
The official releases used for this development ABI are:

| Source | SHA256 |
| --- | --- |
| `https://dovecot.org/releases/2.4/dovecot-2.4.5.tar.gz` | `868c2686a61b5f8e00a3e4721789b1ab46e6528fd773a5fbed07a6ecba7731e6` |
| `https://pigeonhole.dovecot.org/releases/2.4/dovecot-pigeonhole-2.4.5.tar.gz` | `ad7c478cb3aaa76c5f81f86727a3e6843645b0a1253f5684fb8a0beec0d22925` |

Verify the detached `.sig` files with the official
[`DOVECOT-REPO-GPG-2.4`](https://repo.dovecot.org/DOVECOT-REPO-GPG-2.4)
key, fingerprint `EF0882079FD4ED32BF8B23B2A1B09EF84EDC5219`, and check the
archive hashes before extraction. Build prerequisites include a C toolchain,
GNU Make, pkg-config, OpenSSL and zlib development headers, Python 3, and
binutils (`nm`); optional libraries depend on the selected configuration.

For a private absolute prefix `$sieve_prefix`, configure the verified Dovecot
source with `--prefix="$sieve_prefix" --with-shared-libs --disable-static`
and the following minimal optional-feature selection, then `make -j8` and
`make install` **without sudo**:

```text
--without-pam --without-gssapi --without-ldap --without-sql --without-lua
--without-libcap --without-systemd --without-solr --without-flatcurve
--without-textcat --without-stemmer --without-icu --without-sodium
--without-apparmor
--without-bzlib --without-lz4 --without-zstd --without-libunwind --without-pcre2
```

Configure Pigeonhole with `--prefix="$sieve_prefix"`
`--with-dovecot="$sieve_prefix/lib/dovecot" --with-dovecot-install-dirs`
`--without-managesieve --without-ldap --disable-static`. Build/install only
its Sieve library, configuration dependencies and public/generated headers:

```sh
make -j8 -C src/lib-sieve
make -C src/lib-sieve install
make install-dist_pkginc_libHEADERS install-nodist_pkginc_libHEADERS
make -j8 -C src/lib-managesieve
make -C src/lib-managesieve install
make -j8 -C src/plugins/settings
make -C src/plugins/settings install
```

For these dependency compilations, use `CC='ccache gcc'` if ccache is available
and supply an owned `CCACHE_DIR`; preserve deliberately selected compiler flags.
Regex qualification additionally passes `--regex-prefix ABSOLUTE_PCRE2_PREFIX`
and `--mail-utf8` to `build/sieve-deps.py`. The latter enables upstream's
`--enable-experimental-mail-utf8`: without it the pinned conversion rejects
UTF8 locals. Both options and dependency identities enter the signed-build
receipt/cache fingerprint. CI supplies PCRE2-32 development files from its
declared package and enables both options. This qualification configuration
does not install or enable an application mail service.

This installs private dependency files only. It does not register or run a
Dovecot service. The frontend build subsequently needs no network access.
