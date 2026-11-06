# Shared native policy core

The policy consumes immutable snapshots; the classifier returns a borrowed
winning rule and performs no IO. `headers.h` supplies an owned message view from
either a byte buffer or a caller-owned descriptor. Zsh remains the application
default; no Dovecot library is linked here.

The header reader scans once to the empty LF/CRLF separator. Its 1 MiB wire limit
includes line endings and the separator. A physical line payload (excluding its
ending) and each unfolded field value are independently limited to 64 KiB.
Unfolding removes only the line ending, retaining the continuation's SP/HTAB.
Unknown fields still receive syntax, physical-line and unfolded-size checks.
Body bytes, including NUL or arbitrary binary content, are never interpreted.
Descriptor reads use 4096-byte chunks and can consume up to 4095 body bytes
beyond the separator; callers retain descriptor ownership and must not share
its offset. No seek, write, deletion, or message-content modification occurs.

Repeated relevant fields remain separate. Missing relevant fields and empty
groups are valid nonmatches. Empty address field values, malformed lists,
truncation and unsupported address forms are explicit errors, with no usable
partial message. Unfolded Sender values are retained separately for bounce
matching; spam compares after ignoring outer SP/HTAB. Address predicates only
receive extracted mailbox strings, preserving quoted local parts and escaped
bytes. Display names (including encoded words), comments and group names do not
become mailbox values. No display-name or body MIME decoding is needed.

The bounded address-list entry point lives in the existing `rfc822.c` module
and reuses its quote/comment lexer. The legacy `rfc822_parse_adrlist` cannot be
called safely for this contract: its STRING=256 token buffers silently truncate
long mailboxes and its helpers terminate on allocation failure. Audit probes
also demonstrated a dropped escaped quote byte and a null-tail semicolon crash.
Those two defects, a failed-addrspec leak and end-of-string token advancement
were repaired narrowly for legacy callers; fixed-buffer legacy behavior was not
expanded into a new general parser. The native entry point uses caller scratch,
no fatal allocator and no global error state. Its tentative callback output is
published only when the entire header block succeeds.

Supported address syntax covers dot-atom or quoted local parts, ASCII/A-label
domains, CFWS, name/angle addresses, comma-separated lists and nonnested groups,
including empty groups. Source routes, domain literals, non-ASCII domains,
obsolete mixed quoted/dot local forms and malformed separators fail explicitly.
UTF8 local parts are preserved with ASCII-only case folding by policy matching.
No new parser dependency or general MIME/Sieve implementation is introduced.

`snapshot.h` reads immutable compiler inputs and runs abook through an argv-only
descriptor-backed snapshot, once per changed book/exporter identity. It retains
raw provenance paths and length-framed dependency data. Publication locks and
the final source recheck belong to the compiler transaction, not this reader.
`products.h` builds deterministic compatibility arrays, Sieve and cache v1 from
that parsed snapshot. The binary validator checks framing, canonical dependency
order, fingerprint, metadata and both text-product digests. Compatibility source
clears attributes only for its owned variables; readers source an already-open
text descriptor so a sibling `.zwc` cannot replace selected bytes.

The 100000 effective user-rule limit excludes the three mandatory builtin
records. Metadata and dependencies have separate record accounting; the
dependency inventory is bounded to 100000 records independently of user rules.
The 128 MiB cache limit applies to the payload, with 88 additional bytes for
the fixed header. Text buffers retain their separate 128 MiB ceiling and input
reads retain their 64 MiB ceiling. Boundary arithmetic is checked before adding
or allocating. Cached contact exports are reused only when their own digest,
raw book, exporter/compiler bytes and links, flags, locale, and normalization /
schema identities all agree.

The first supported export profile is `core-v1`. Its built-in defaults declare
fileinto/mailbox and enable mailbox creation. An empty profile uses these
defaults. Nonempty profiles must name version/profile and every required
capability; `create-mailboxes no` needs only
fileinto. Mailbox mappings are final names; named destinations otherwise use the
configured prefix and separator. Colliding effective destination mappings fail.
Legacy ERE export requires the explicit `pigeonhole-2.4.5-posix-ere-v1` profile,
regex capability and exact digest of the compiler-bound installed catalogue.
Only its ASCII concatenation grammar is admitted; patterns are emitted unchanged.
Unsupported syntax fails with a source-line migration diagnostic. The catalogue
qualifies actual generated scripts against independent native/Sieve winners,
using a regex/SMTPUTF8-enabled signed 2.4.5 development oracle. Tools remain
test dependencies only. No caller-provided digest can create new equivalence.

Compiler v2 uses `mailbox-minimal-quoting-v2` across typed predicates,
contacts/own inputs, raw headers and direct parsed messages. Decode redundant
local quotes/quoted pairs, then serialize with necessary quotes and escaping.
ASCII case policy, domain grammar and regex patterns are unchanged. This fixes
the initial quoted-spelling assumption; it does not change the legacy shell
engine. Normalization bytes enter fingerprints and required cache metadata;
prior compiler/normalization generations are incompatible, status 5.

`generation.h` implements the immutable directory reader and compiler
transaction. Shared/exclusive POSIX record locks serialize snapshots and commit;
a busy lock returns status 6. Readers keep one verified directory descriptor.
Writer initialization fsyncs generations, engine, cache, then root before any
publication, including on retries with an already existing hierarchy. This
persists each new directory entry in its immediate parent; legacy cache modes
are retained. Read-only checks and pinned readers never flush the hierarchy.
A prepared journal fences readers while the public export and selector change.
On recovery, an uncommitted journal restores the prior export bytes/mode and
selector; a durable committed marker retains the new generation. Committed
journals are readable only after checking their selector and complete public
and immutable products. The next exclusive invocation finishes cleanup. Old
generations are never removed by routine compilation. Test hooks are compiled
only into separate test executables, never the production compiler.
Installation removes stale compiled Zsh module caches before dispatching the
new wrappers; compatibility arrays are still read through an open text fd.

`make -f build/filter-core.mk filter-compile` also builds
`build/gnu/filter/jaro-filter`. It selects an immutable generation without
compiling inputs, reconstructs its validated canonical R records into the same
opaque indexed policy, and compiles each ERE once under a per-thread C locale.
Stored IDs, resolved order, provenance and local/Sieve destinations survive
reconstruction. No raw policy source is reparsed. Allocation failure destroys
the entire reconstructed model; no partial policy escapes.

The helper defaults to classification and emits the byte-framed JFD1 stream
specified in `doc/filter-interfaces.md`. Maildir selection snapshots regular
files from cur/new only, sorts raw absolute paths and excludes tmp, directories,
symlinks and special files. Explicit paths preserve their original bytes and
occurrences; relative paths retain the invocation's cwd. Selection is bounded
to 100000 paths and 64 MiB including their NUL terminators. A complete path list
and generation are validated before any decision output. An empty selection
emits only the header. Empty/malformed or unreadable messages produce error
records and aggregate status 7, while later selected messages are still read.
Source identity and size/mtime/ctime are rechecked around the single header
read. New names after selection are deferred; replaced, renamed or changed
selected files become errors. A source in its destination's cur/new is `noop`
with action `none`, retaining the winning rule and destination metadata.

Classification opens messages read-only and never changes mail, books or cache
products. Reads may update filesystem access times. Explicit `--generation ID`
allows prior compatible snapshots, even when input sources have changed. Current
source fingerprint checks are performed by the read-only CLI workflows. Explicit
`--mode apply` uses the storage adapter described below. The existing shell
engine remains the application default; explicit `JARO_FILTER_ENGINE=native`
dispatches filter/fetch to this helper, with one compile and pinned batch.
`zsh extras/test/test-filter-batch.sh` exercises independent target/Sieve winners,
raw byte paths, complete snapshots, framing/cache failures, allocator cleanup
and deterministic pinned-generation/source-change barriers in disposable mail.
Native test suites accept `JARO_FILTER_TEST_TMP` as an explicit parent for their
temporary fixtures, without changing inherited TMPDIR/TMP/TEMP. Without a suite's
retained `*_WORK` override, these owned fixtures use automatic cleanup; test IDs,
counts and failures remain in the outer runner's retained logs. A retained work
override continues to preserve complete fixture data and command logs.

`storage.h` owns mutating Maildir IO. After complete list/generation validation,
apply snapshots identities for every explicit occurrence and takes a nonblocking
exclusive POSIX lock on persistent `cache/filter-engine/batch.lock` (0600, owned
regular file, one link). It never unlinks that inode; closing its own descriptor
releases its lock, including after a crash. Classification takes no batch lock.
Different roots serialize independently; source claiming still prevents a
second batch from unlinking a replacement. New names after selection are deferred.
Each initially selected inode gets at most one apply attempt per batch, including
explicit aliases/repeated paths; later occurrences are errors after that attempt.
Successful no-ops can repeat. This prevents a later occurrence consuming a new
arrival even if the filesystem recycles an inode number and timestamps.
The root, source parent/file, and destination directories are descriptor-pinned.
Destination components are traversed without symlinks or `..`; missing relative
Maildirs are created privately with fsync of each immediate parent.

Same- and cross-filesystem transfers follow the same bounded copy path:

1. Create an exclusively owned 0700 directory under destination tmp and a 0600
   message inside it. Copy through a 64 KiB buffer from the already-open source
   descriptor. Verify the copied header prefix against the parser's exact wire
   header digest, fsync the copy, and read it back to verify its full SHA256.
2. Hard-link that complete file into destination cur, never replacing a name.
   Try the original basename first; collision alternatives preserve the entire
   raw `:2,` suffix. Creation/publication each stop after 64 attempts. No original
   path, flag suffix or predicate is truncated. Fsync the published file and cur,
   then remove only this invocation's private staging entry and sync its parents.
3. Atomically rename the current source entry into a newly created, owned 0700
   directory in its original parent. Verify the claimed entry against the pinned
   source descriptor. Only this process writes that private namespace; ordinary
   Maildir clients continue operating on cur/new names. The mutable original
   source name is never passed to unlink. A replacement captured by the rename
   fails verification and is restored without overwriting a later arrival.
4. Sync the source claim and parent; recheck the destination and both full byte
   digests before unlinking the owned claim. Sync the source directory changes
   and remove the empty owned directory. Success leaves one intact destination
   copy and no owned tmp/claim entry. A source already in the destination's
   actual cur/new directory is an inode-preserving no-op.

Linux rollback uses `renameat2(RENAME_NOREPLACE)` for every entry type. The
portable fallback uses no-clobber hard links for nondirectories; an unexpected
directory replacement is retained in recovery when that rollback is unavailable.
This fallback has not been qualified on non-Linux hosts. Published entries are
never removed during rollback: an independent Maildir client may already have
renamed them. On a source replacement/flag rename before claiming, or an observed
byte edit, return error/status 7 and retain recoverable bytes. A new entry placed
at the original name after a verified claim is independent and stays untouched.

Maildir delivery requires immutable message contents after publication. Full
checksums detect edits at the tested boundaries, including same-size writes
that timestamp checks miss; arbitrary pre-opened writers or actors entering this
process's private directories are outside that ownership protocol. This is not
filesystem transaction isolation against such writers. Source content, mode,
owner, ACLs and xattrs are never modified in place; the destination is a new
0600 file with unchanged message bytes and flags.

Any read/write/verify/fsync/unlink/cleanup failure remains nonzero. Error records
retain a known winning rule ID/provenance, use action none, and leave destination
fields empty. Stderr emits exact byte-escaped `E_RETAINED_PATH` locations for
retained published, claimed or staging entries. A failed operation can leave a
complete duplicate, including when source unlink succeeded but a later fsync
failed. Reruns cannot overwrite messages and never adopt another invocation's
tmp/claim entries. They do not deduplicate retained copies automatically.
INT/TERM/HUP/PIPE request owned cleanup and conventional 128+signal status.
SIGKILL can leave partial tmp files (never published), or a source claim plus an
already durable destination. Those paths remain recoverable; later batches
leave them alone. No garbage collection or power-loss validation is claimed.

`zsh extras/test/test-filter-storage.sh` checks all 52 authored target IDs and
actual Sieve winners in apply mode, real same/cross-filesystem full-body hashes,
flags/collisions, exact survivor/duplicate counts, injected failures, private
claim and rollback boundaries, persistent locks, source/destination replacements,
observed edits, graceful interruption and SIGKILL recovery. It uses only owned
synthetic fixtures and settles every controlled child.
