# Minimal Sieve tool source extraction

Only the files needed for the **sievec** and **sieve-test** command frontends
are extracted from Pigeonhole 2.4.5, pinned to Git commit
`742bb8923b5aab398f8d962e827fcde7ad4a3f73`:

| Upstream path under `pigeonhole/upstream/` | Purpose |
| --- | --- |
| `src/sieve-tools/sievec.c` | Compiler entry point |
| `src/sieve-tools/sieve-test.c` | Fixture evaluator entry point |
| `src/lib-sieve-tool/sieve-tool.c` | Shared command implementation |
| `src/lib-sieve-tool/sieve-tool.h` | Shared command interface |
| `src/lib-sieve/util/mail-raw.h` | Private raw-message interface used by the shared implementation |
| `AUTHORS`, `COPYING`, `COPYING.LGPL` | Attribution and licensing notices |

No release archives, full upstream trees, server implementations, extra command
frontends, translations, generated build systems, or upstream test suites are
vendored. Each retained file has a dependency reason and SHA-256 digest in
[`sources.json`](sources.json). The selected bytes were checked against both
the pinned upstream Git commit and the previously signature-verified release.
The release URL, digest, and signer fingerprint remain provenance metadata;
archive signature verification is not an offline test of this extraction.

## Build boundary

This is a minimal frontend source slice, **not a standalone Sieve interpreter**.
It requires matching Dovecot core and Pigeonhole **2.4.5 development headers and
libraries** supplied as test-build prerequisites outside the vendor tree.
The shared helper links to `libdovecot-sieve`, `libdovecot-storage`,
`libdovecot-lda`, and `libdovecot`. The retained `mail-raw.h` pins the private
raw-mail interface even when development packages omit that header. Its functions
come from the matched Sieve library, so their implementation is not duplicated.

The opt-in `make sieve-tools` build checks versions and required symbols before
compiling and uses the selected sources and matched generated development headers.
See [the private tool build instructions](../doc/sieve-tools.md) for prerequisites,
fingerprints and private output paths. If development dependencies
are built from upstream sources, keep those source/build caches outside this
repository; never expand the vendor tree to a full distribution as a workaround.
Additional vendored source requires a demonstrated compile/link dependency,
per-file provenance, a reason, and an updated minimal-scope regression.

The ordinary application build does not compile these tools. This development
dependency does not add a Dovecot runtime requirement to ordinary Jaromail mail
processing. Isolated semantic fixture configuration is covered separately from
frontend compilation.

## Verification and maintenance

```sh
python3 vendor/verify.py
```

This offline check needs Python 3. It verifies every selected file's size and
digest, rejects symlinks and unexpected files, and rejects archives or source
trees outside the explicit extraction. The source regression also injects
modified, missing, and extra files to verify that each is rejected.

For each manifest entry, the source URL is:

```text
https://raw.githubusercontent.com/dovecot/pigeonhole/<commit>/<path>
```

To refresh the extraction, select an explicit upstream commit, fetch only the
allowlisted paths, review changes and licensing, verify/update their digests,
and rerun integrity and tool-behavior checks. Retain source files verbatim;
keep any necessary local patches separately with justification and tests.

Pigeonhole's [`COPYING`](pigeonhole/upstream/COPYING) declares LGPL-2.1 by default,
subject to per-file notices. Retain [`COPYING.LGPL`](pigeonhole/upstream/COPYING.LGPL)
and [`AUTHORS`](pigeonhole/upstream/AUTHORS). Jaromail's license does not replace
these upstream terms.
