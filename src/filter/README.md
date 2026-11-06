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
