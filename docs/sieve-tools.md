# Pinned Sieve tools and runtime

A native Linux build configured with `SIEVE_DEPS_PREFIX` or the CMake cache
path `JARO_SIEVE_DEPS_PREFIX` installs a private runtime. The compiler and
binary loader use the packaged configuration and libraries. They run as the
invoking user. Neither operation requires a Dovecot server or mail directory.

```sh
cmake -S . -B out/sieve -DCMAKE_BUILD_TYPE=Release \
  -DJARO_SIEVE_DEPS_PREFIX=/absolute/private/development-prefix
cmake --build out/sieve --parallel
cmake --install out/sieve --prefix "/tmp/jaro install"
runtime="/tmp/jaro install/share/jaromail/sieve"
"$runtime/run" compile Filters.sieve Filters.svbin
"$runtime/run" dump Filters.svbin
"$runtime/run" identity
```

`compile` publishes the output after successful compilation. `dump` opens the
named binary with Pigeonhole's `sieve_load` and needs no source file.
`identity` verifies the installed byte inventory and prints the SHA256 of
that inventory. The launcher checks it before either tool runs, then invokes
the packaged `doveconf` with an explicit configuration, settings module and
library directory. Pigeonhole's bytecode belongs to that host runtime.

When publishing a policy, `jaro-filter-compile` resolves the runtime relative
to its own executable. Set `JARO_SIEVE_RUNTIME` to the absolute directory
containing `run` and `SHA256SUMS` to use an existing package. The generation
manifest records the input fingerprint, source and executable digests, runtime
inventory digest and profile digest. An unchanged update validates the
published products and reuses the executable.

The same native installation provides `jaro-sieve-filter`. Its launcher
requires Python 3 and procfs. The Maildir worker uses Linux Landlock ABI 3
or newer (Linux 6.2 or a backport) to confine the storage driver's file access.
Its build requires Linux UAPI headers with Landlock truncate support. When
the kernel or process sandbox cannot activate the ruleset, the command exits
before opening the source mailbox.

Run a batch with `jaro-sieve-filter [--dry-run] --from INCOMING --to BASE SCRIPT`.
Both roots must be absolute directories. The build uses GNU patch and the
Linux no-replace rename interface. The worker pins one executable for the
batch. The launcher snapshots the initial regular files in `cur` and `new`
and locks the source directory against other filter batches. Preview evaluates
a private read-only view. A previewed save never creates a mailbox, so a
later `mailboxexists` test can choose a different folder during apply.
Workflow batches accept up to 100,000 messages and reject larger inventories
before claiming mail. Apply claims each occurrence by rename within the
source filesystem before Pigeonhole evaluates it. A keep or fileinto naming
the original mailbox restores the same name and bytes in the original directory.

Dovecot commits each destination save before expunging the corresponding claim.
Moves publish in the target's `cur` and preserve the source filename, flags
and complete byte stream. Published copies have mode 0600 and preserve modification time to the second. On a name collision, the private Maildir driver
uses a fresh basename with the original flag suffix. A failed occurrence
returns a nonzero batch status and leaves other completed deliveries in place.
Ordinary failure and handled interruption return unprocessed mail to the
source. A competing same-name arrival is preserved too.

Pigeonhole reconstructs envelope tests from the delivered headers. It tries
`Return-Path`, `Sender`, then `From` for the sender, and `Envelope-To` then
`To` for the final recipient. Its original recipient defaults to that final
recipient. Missing addresses fall back to `sender@example.com` and
`recipient@example.com`. SMTP-time envelope information absent from the
stored message cannot be reconstructed.

An unhandled process death, failed restoration or interrupted source commit
can leave recoverable files in `INCOMING/tmp/.jaro-sieve-transaction`.
Further apply attempts stop while this directory exists. Inspect its
`inventory.jsonl` to map each remaining numeric file in `cur` or `tmp` to
its original directory and filename. Compare it with source and destination
copies before restoring mail under an unoccupied name. A process death after
destination commit can leave both copies. Once all occurrences are accounted
for, remove the empty transaction directories and inventory before retrying.

| Distribution | Link and load dependencies |
| --- | --- |
| Native Linux, matched shared runtime | Private `libdovecot{,-storage,-sieve,-lda,-managesieve}.so.0`, `libpigeonhole_settings.so` and `doveconf`. The Maildir driver is compiled into `libdovecot-storage`. The qualified x86-64 glibc profile uses the host ELF loader, libc, libm, zlib and PCRE2-32. The launcher uses POSIX sh and GNU coreutils. |
| Linux release archives, x86-64 and arm64 | The musl helpers are static. The same archive includes the private Sieve component built on a native Ubuntu 24.04 runner for that architecture. Filtering needs its glibc loader, Python 3, zlib, PCRE2-32 and Landlock support. |
| macOS and other native targets | The private ELF runtime build is available on Linux. |

Without a Sieve prefix, CMake builds the existing application helpers. The
standalone `make sieve-tools` target builds the pinned compiler, simulation
tool, binary dumper and upstream Maildir frontend against the supplied
development installation. The private runtime includes the compiler, binary
dumper and JaroMail's Maildir adapter.

```sh
SIEVE_DEPS_PREFIX=/absolute/private/development-prefix make sieve-tools
cat build/sieve-tools/current/build.json
```

The prefix must contain `include/dovecot/config.h`, `dovecot-version.h`, the
Dovecot public headers, `include/dovecot/sieve` including generated
`pigeonhole-config.h` and `pigeonhole-version.h`, and the shared development
libraries `lib/dovecot/libdovecot{,-storage,-lda,-sieve}.so`. Pigeonhole must
match revision `742bb8923b5aab398f8d962e827fcde7ad4a3f73`. A distribution's
2.4.1 libraries or system `sievec` binaries are not substitutes.
The local delivery workflow runs without a server. The separate
`extras/test/test-sieve-lmtp.sh` interoperability test starts a private
Dovecot LMTP service from the development prefix on a Unix socket. Its
synthetic recipient receives the same messages as the staged client.

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
private tools. The CMake runtime build uses its own fingerprinted build output.

### Optional regex oracle prerequisite

The minimal default dependency recipe disables PCRE2, so its regex extension
reports missing regular expression support. To investigate and qualify regex
exports, pass `--regex-prefix /absolute/private/pcre2-prefix` to
`build/sieve-deps.py`. That prefix must contain `include/pcre2.h` and one
`libpcre2-32.so` development link under `lib` or an architecture subdirectory,
resolving inside the same prefix. No system installation is performed.

The option uses the same signed and pinned Dovecot/Pigeonhole archives, enables
`--with-pcre2`, and records PCRE2 header/library hashes, link flags and runtime
search path in the build identity. The fixture evaluator uses the same pinned runtime. PCRE2 support alone does not qualify POSIX ERE equivalence: an
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
`--without-managesieve --without-ldap --disable-static`. The development
prefix needs the Sieve library, configuration dependencies, public/generated
headers and the delivery plugin for the LMTP fixture.

```sh
make -j8 -C src/lib-sieve
make -C src/lib-sieve install
make install-dist_pkginc_libHEADERS install-nodist_pkginc_libHEADERS
make -j8 -C src/lib-managesieve
make -C src/lib-managesieve install
make -j8 -C src/plugins/settings
make -C src/plugins/settings install
make -j8 -C src/plugins/lda-sieve
make -C src/plugins/lda-sieve install
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
