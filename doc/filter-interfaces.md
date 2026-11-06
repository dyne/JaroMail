# Native filter interface contract v1

The compiler, batch helper and native-default CLI dispatch implement the
contracts below. Explicit legacy selection remains available for rollback. Policy
order and independent target decisions are in [filter-policy.md](filter-policy.md).
The first implementation remains two executables sharing `src/filter/` modules,
with Zsh retaining command routing, account setup, transports and indexing.
No daemon, service protocol or general Sieve interpreter is required.

## Commands and statuses

```text
jaro-filter-compile --root ROOT [--accounts-dir DIR] [--profile FILE]
                    [--check] [--report0]
jaro-filter-compile --root ROOT --import-sieve FILE [--legacy-generated]
                    [--accounts-dir DIR] [--profile FILE] [--check] [--report0]
jaro-filter --root ROOT [--generation ID] [--mode classify|apply]
            (--maildir DIR | --files0-from FILE | -- PATH...)
```

`--root` is required and nonempty. Helpers do not infer roots from cwd/HOME.
Wrappers resolve JAROMAILDIR, JAROWORKDIR and JAROACCOUNT as today, passing
explicit arguments. Defaults are ROOT/Accounts and ROOT/FilterProfile.txt.
An absent profile selects `core-v1`; an explicitly named unreadable profile
is an error. Repeated singleton options, unknown flags, missing/empty values,
an empty positional path, or mixed selection forms fail with status 2.
`--` ends option parsing; `-- -message` is a literal relative path.
`--help` and `--version` need no root and return 0 without writes.

The compiler snapshots rules, accounts/aliases and abook exports once, validates
all outputs, and publishes one generation. `--check` performs the same policy
and export validation without publication or input mutation. `--report0` emits
`JFC1 NUL generation-id NUL changed NUL generation-directory NUL`; changed is
ASCII `0` or `1`. A check reports an empty directory and changed `0`. Otherwise
compiler stdout is empty, including on error. Diagnostics go to stderr and
identify a stable error code, source path (byte-escaped for display), physical
line where available, and offending selector/profile. Never execute input text.

The batch default is `--mode classify`. Only explicit `apply` changes messages.
`--maildir` selects regular message files from cur/new only, snapshotting their
names in byte-sorted order. `--files0-from -` reads stdin; other values name a
binary list file. A literal file named `-` can be spelled `./-`. Explicit paths
retain input order and repeated paths get separate occurrence indices; apply
revalidates each occurrence and reports a later missing/replaced occurrence as
an error rather than moving an unrelated replacement. Validate the complete
path-list framing and generation before any apply operation. An empty list or
Maildir succeeds with a header-only decision stream.

| Exit | Meaning |
| --- | --- |
| 0 | Complete success; includes empty batches and valid no-change compilation |
| 2 | Invocation/path-list framing error; no application started |
| 3 | Global IO/resource failure, including unreadable config or failed publication |
| 4 | Invalid policy, unsupported capability/export/import grammar; no publication |
| 5 | Missing, stale, incompatible or corrupt generation; no application started |
| 6 | Busy lock or inputs changed during snapshot/recheck; retryable, no publication |
| 7 | Batch completed with message-level failures; successful records remain successful |

Signals retain conventional 128+signal status. A missing executable can produce
shell status 127; wrappers must propagate it. Helpers never turn errors into
success by running a different engine. Empty/malformed messages are retained
and produce an error record/status 7; they are not silently removed. A partial
stream or nonzero exit must never be treated as complete successful delivery.

## Byte framing and decision records

POSIX argv and filesystem paths are byte sequences excluding NUL. Preserve
spaces, tabs, newlines, leading dashes and non-UTF8 bytes without decoding them.
The input list is zero or more **nonempty path + NUL** items. EOF after a partial
item, an empty item, or a size-limit violation fails before apply. Relative
paths are resolved against the initial cwd, never against a changing cwd.

Batch stdout starts with `JFD1 NUL generation-id NUL`. It then contains fixed
eleven-field records, each field terminated by NUL, in this order:

| Field | Encoding / meaning |
| --- | --- |
| index | ASCII decimal, 1-based occurrence index |
| path | Original raw path bytes; Maildir enumeration uses absolute raw paths |
| rule_id | ASCII stable ID; empty when no classification was possible |
| action | `move` or `none` |
| logical | Logical destination, or empty for an error |
| local | Absolute destination Maildir path bytes, or empty for an error |
| sieve | UTF8 mailbox name, or empty for an error |
| status | `classified`, `moved`, `noop`, or `error` |
| source | Raw rule provenance path, empty for builtins |
| line | ASCII decimal physical line, or `0` when not applicable |
| detail | Empty on success; stable ASCII error code on failure |

There is no newline delimiter and no JSON conversion of path bytes. Split only
on NUL, retain empty fields, and require a complete multiple of eleven fields
after the two-field header. Generation IDs are 64 lowercase hex digits. A
consumer must also check process exit status; it cannot infer success from a
valid prefix. `noop` uses action `none` and means source already equals destination,
with bytes and flags unchanged. Applying an error record never authorizes deletion. Native
human explain/dry-run output is rendered by the Zsh wrapper from these records;
helper stdout contains no progress, message body, Sieve text or diagnostics.

Apply failures after classification retain the winning rule ID and provenance,
use action `none`, and leave logical/local/Sieve fields empty. Stderr may name
retained copies with `E_RETAINED_PATH` and a complete byte-escaped quoted path.
Publication is no-clobber through destination tmp; source removal uses an owned
private claim after destination durability. See the storage and concurrency
contract in [src/filter/README.md](../src/filter/README.md). A nonzero apply can
retain complete duplicates; it never authorizes treating the whole batch as
delivered or removing those copies blindly.

Limits: 64 MiB per policy input/export, 64 KiB per rule line/predicate, 100000
effective rules/contact predicates, 100000 selected paths/64 MiB of path bytes
including NUL terminators, 128 MiB cache payload,
and 1 MiB total message headers/64 KiB individual header line. Reject excess
before publication; a per-message header limit failure retains that message.
Do not truncate a selector, mailbox, path or message to fit a limit. The OS's
path limits still apply and failures are explicit.

## Policy text, identities and profiles

Filters.txt is UTF8 line-oriented text; LF and CRLF are accepted. Outside
double quotes, ASCII space/tab separates tokens; a `#` starting a token starts
a comment. Bare tokens preserve backslashes literally. A double-quoted token
uses JSON string escapes, including escaped quote/backslash; decoded NUL is
forbidden. Empty predicates/destinations fail. There is no shell expansion,
single-quote syntax, command substitution or general glob interpretation.
For example, `to-address team@example.org move "Project People"` is one route;
`from "^list\\+news@example[.]org$" save lists` preserves the ERE backslash.
Destinations are relative UTF8 Maildir paths: no absolute path, empty/dot/dotdot
component, NUL/control byte or CR/LF. Spaces are allowed via quoting.

Use the policy's stage order. Retain the last definition of an identical
selector/canonical predicate, at that definition's position. Normalize save to
move before identity generation. For EREs, canonical predicate means original
pattern bytes, not a rewritten regex. Other predicates use their declared
address/domain normalization. Known exact addresses precede known-domain
records for ID tie resolution; domains retain Filters.txt order. Both yield
known. Contact duplicates collapse by normalized address. Own-account entries
precede redundant aliases for provenance, then byte-sorted account path/line
and alias file order determine provenance. Same-stage distinct explicit routes
always use source order, regardless of address enumeration order.

Builtins use `builtin:bounce`, `builtin:spam`, `builtin:unsorted`. Contact and
own IDs use `blacklist:`, `known:` or `own:` followed by SHA256 of the normalized
mailbox. Explicit IDs are `r:` plus SHA256 of fixed NUL-separated fields:
selector, canonical predicate, normalized action, logical destination, source
identity, occurrence number among identical canonical records. Source identity
is the root-relative path (`Filters.txt` normally), not the absolute mail root;
an external account source uses `accounts:` plus its directory-relative path.
Physical lines are separate provenance and do not enter IDs. Inserting blank
lines or unrelated rules therefore does not change an existing rule's ID.
The target fixture aliases `filters:N`/`domain:N` name expected source records;
later tests resolve them to these IDs rather than changing expected winners.
Contact fixture aliases `known:mailbox`, `blacklist:mailbox` and `own:mailbox`
resolve by hashing that canonical mailbox into the corresponding serialized ID.
For explicit IDs join the six decoded byte fields with one NUL and no trailing
NUL; occurrence is 1-based ASCII decimal. Contact IDs hash mailbox bytes only.

Legacy ERE matching uses `REG_EXTENDED|REG_ICASE`, LC_ALL=C, no PCRE, compiled
once per loaded policy/process. This pins the actual shell's CASE_MATCH-off
behavior. It does not license translating an ERE into a Sieve substring/glob.
Native parsed mailboxes first use `mailbox-minimal-quoting-v2`: decode redundant
local quotes/quoted pairs and re-quote only when dot-atom serialization is
insufficient, escaping embedded quote/backslash. Necessary spaces and quoting
survive. Empty locals fail; supported UTF8 locals are not Unicode-folded.
Typed predicates, contacts/own inputs and both raw/direct message APIs share
this normalization. Patterns retain their original regex meaning over these
canonical strings. This is a narrow representation correction relative to the
earlier native assumption and legacy shell spellings, documented in
[filter-policy.md](filter-policy.md).

The address-case policy is `ascii-mailbox-casemap-v1`: typed exact, known,
blacklist and own predicates ASCII-fold the entire parsed mailbox. Sieve uses
`address :all :is :comparator "i;ascii-casemap"` on the prescribed field list.
Do not combine localpart/domain tests across addresses. Domain predicates use
`:domain` and the same comparator; they still enforce the ASCII label grammar.
This choice and its version enter the cache metadata/fingerprint and survive
export/import. The whole-string comparator semantics are defined in
[RFC 5228 sections 2.7.3–2.7.4 and 5.1](https://www.rfc-editor.org/rfc/rfc5228.html#section-2.7.3).

FilterProfile.txt uses the same token lexer, with these records:

```text
version 1
profile core-v1
capability fileinto
capability mailbox
create-mailboxes yes
namespace-prefix ""
namespace-separator "/"
mailbox known INBOX
mailbox spam Spam
```

Singleton records cannot repeat; capability and mailbox keys must be unique.
Unknown records/capabilities fail. `create-mailboxes no` uses plain fileinto;
yes requires mailbox and emits :create. Prefix/separator are explicit UTF8
strings without control bytes. Named destination components are joined with
the configured separator and prefix. Explicit mailbox mappings are final names,
not prefixed again. Defaults: known -> local known / server INBOX; spam -> local
zz.spam / server Spam; all other names retain their local relative path and use
the configured server namespace. Builtin zz.blacklist, zz.bounces, priv and
unsorted follow that named-destination rule. No provider capability is inferred.
The builtin spam record's logical value is `spam`, its local value is `zz.spam`;
an explicit destination token `zz.spam` is instead a named destination and keeps
that server name unless explicitly mapped. The strings are not silently aliased.

`core-v1` exports typed exact/address/domain predicates with base address tests
and fileinto, plus mailbox only when :create is enabled. The optional
`pigeonhole-2.4.5-posix-ere-v1` profile additionally declares regex and a
`regex-qualification SHA256` record identifying independent pinned-oracle
evidence for its supported ERE subset. An undeclared, unqualified or unsupported
ERE fails with status 4 and a migration diagnostic. Qualification is not a
blanket assertion that every POSIX regex/comparator behaves equivalently.
The qualification digest must match an installed, compiler-version-bound
qualification catalogue; a caller-supplied hash cannot assert new equivalence.
Runtime validation is native structural/profile validation. The independently
pinned sievec/sieve-test tools remain build/test oracles, not application
dependencies.

The installed [qualification catalogue](filter-regex-qualification.json) binds
compiler `jaromail-filter-compiler-v2`, normalization v2 and the independent
corpus to the ASCII concatenation grammar: nonempty printable ASCII literals,
dot, `[.]`, escaped regex metacharacters, and optional outer `^`/`$` anchors.
No groups, alternatives, repetitions, classes, backreferences or engine escapes
are admitted. Patterns are emitted unchanged as `address :all :regex`.
Qualification uses signed 2.4.5 sources configured with PCRE2-32 and
`--enable-experimental-mail-utf8`, so supported UTF8 mailbox extraction agrees
too. A backend without that configuration is outside this profile's assurance.
The profile must explicitly declare regex capability and the exact installed
catalogue SHA256; unused regex requires are omitted. Runtime never invokes
Dovecot tools. Unsupported syntax reports status 4 with the rule's source line,
preserving the prior generation/export.

## Cache and publication

The canonical root is ROOT/cache/filter-engine. A generation directory is
`generations/<fingerprint>/`, containing exactly `policy.bin`, `filters.zsh`,
`Filters.sieve` and diagnostic `manifest.json`. Directories are private (0700),
files 0600. No .zwc file is generated or read by v1. Compatibility readers pin
the generation and source its already-open text descriptor, for example
`exec {fd}<"$generation/filters.zsh"; source /dev/fd/$fd; exec {fd}<&-`.
This prevents an old sibling .zwc from overriding selected source bytes.
The legacy cache ROOT/cache/filters remains a separate legacy-only artifact.
Compatibility text clears prior attributes of its owned variables and declares
filter_from/filter_to maps without lowercase attributes, the existing own/
whitelist/blacklist arrays, and parallel filter_rule_ids, filter_rule_kinds,
filter_rule_patterns, filter_rule_destinations, filter_rule_stages and
filter_rule_masks arrays in resolved rule order. Legacy maps contain ERE routes;
the ordered arrays retain every typed rule too. Quote every value literally;
neither dollar signs nor backticks/substitution syntax in data may execute.

`policy.bin` has an 88-byte header: ASCII `JFNCACHE` (8 bytes), little-endian
u32 cache version 1, u32 policy version 1, u64 payload byte length, 32 raw
fingerprint bytes and 32 raw SHA256 payload digest bytes. Payload starts with
u32 record count. Each record has a one-byte tag, u32 field count and fields
encoded as u32 byte length followed by exactly that many bytes. No pointers,
host-sized integers or serialized regex_t objects are stored. Reject unknown
tags/versions, duplicate metadata, bad counts/digests, excess lengths and trailing
bytes before use. Records are:

| Tag | Fields in order |
| --- | --- |
| M (one, first; 7 fields) | compiler build ID, profile ID, regex dialect, address-case policy, Sieve address comparator, raw32 Zsh SHA256, raw32 Sieve SHA256 |
| D (dependency; 5 fields) | role, raw source path, ASCII presence 0/1, raw32 digest or empty for absent input, auxiliary snapshot bytes (export/link/inventory when needed) |
| R (rule; 12 fields) | ordinal, stage, kind, field mask, rule ID, predicate, logical destination, local relative destination, Sieve mailbox, raw source path, physical line, action |

Rule ordinals/line/stage/mask are ASCII decimal. Stages are 1 through 8; field
mask bits are From=1, Sender=2, To=4, Cc=8, X-Spam-Flag=16. Kind is one of
address, domain-apex, domain-subdomains, ere, sender-contains, spam-is, always.
Builtins are explicit records; rule order is already resolved. Predicate and
destination strings exclude NUL; dependency auxiliary data can contain any
bytes. Readers verify both generated text digests recorded in M before using
a generation. Native matching reads immutable policy.bin, compiling its EREs
once; manifest.json is diagnostic, not another independently mutable authority.
M's address-case value is exactly `ascii-mailbox-casemap-v1`; its regex dialect
is exactly `posix-ere-c-icase-v1` and its Sieve address comparator is exactly
`i;ascii-casemap`. Unknown values are incompatible, status 5.
The payload order is M, D records byte-sorted by (role,path), then R records in
1-based ordinal order. Duplicate dependency keys or noncanonical order fail.
Compiler v2 requires `config.address-normalization` with exact value
`mailbox-minimal-quoting-v2`. Prior compiler/normalization metadata is
incompatible (5); it cannot masquerade as a generation of the corrected model.

Fingerprint SHA256 covers a versioned, length-framed ordered input inventory:
raw Filters.txt/aliases bytes and presence, byte-sorted account file paths/content
and membership, raw whitelist/blacklist bytes and canonical contact export
bytes, exporter executable digest/flags, compiler executable/build ID, cache/
policy versions, regex locale/dialect, parser/address normalization versions,
whole-address comparator identifier,
profile content/capabilities/qualification and mailbox namespace/mappings.
Follow explicitly configured input symlinks only as input: fingerprint/recheck
both link target bytes and opened regular-file contents. Output/message symlinks
are not followed. Missing optional books/aliases/accounts mean explicit empty
inputs; missing Filters.txt is an error. Inventory membership changes invalidate
even when mtimes are preserved. Directory membership and resolved inputs must
be rechecked before publication.
Precisely, hash ASCII `JFFP1` plus NUL followed by each complete sorted D record
in its binary payload encoding. Source-content digests cover complete raw bytes;
presence distinguishes an absent file from an empty file. Effective compiler/
schema/normalization/profile/comparator/namespace values are required D records
too, with ASCII `config.` role names, empty path, presence `1`, their value's
SHA256 and value bytes as auxiliary data. Input roles use `input.` plus the
source kind, exports use `export.whitelist`/`export.blacklist`, account membership
uses `inventory.accounts`, and link records use `link.` plus their input role.
Account-file records share role `input.account` and retain distinct raw paths.
This framing prevents concatenation ambiguities and makes the fingerprint
independently reproducible without trusting mtimes or native struct layout.

Export each abook at most once when its raw content or exporter identity changes.
Books containing only blank lines and `#` comment lines have an empty export,
as do those with only the complete ordered header `[format]`, `program=abook`,
`version=0.6.0` or `version=0.6.1` (allowing blank/comment lines between them).
Space, tab and CR padding is ignored for this recognition; NUL and other control
bytes are excluded. This includes Jaromail's touched books and comment-only
fixtures, which abook itself rejects for having no records. Other input still
goes through abook with its normal failure checks. The exporter must remain
available, and the original book bytes remain unchanged and fully fingerprinted.
An unchanged verified book can reuse its last generation's stored export bytes;
include their digest in the new fingerprint. Never trust mtime alone. Snapshot
all inputs under a shared publication lock, release that lock, build in an owned
private sibling staging directory, validate the
binary, literal-safe compatibility arrays and capability-qualified Sieve, then
recheck source content/membership under the compiler's exclusive publication
lock. Retry a changing snapshot at most twice, then status 6. A no-change hit
still validates selected output integrity and schema before returning changed=0.

Rename the completed immutable directory into generations, then atomically
replace `current` with a relative symlink `generations/<64hex>`. Readers take a
shared publication lock, read current once, validate its exact relative target,
open that generation directory without following replacement links, then release
the lock and retain the directory descriptor for the entire batch. Open all
products relative to that descriptor. Concurrent publication cannot mix products
or change a running batch's policy. No normal invocation garbage-collects old
generations; only its own abandoned staging files may be removed.

ROOT/Filters.sieve remains the public compatibility export. Its atomic whole-file
replacement is part of the same publication lock/journal transaction as the
selector update. Prepare a durable journal identifying prior selector/export
backup and proposed generation before either public view changes. Readers refuse
an unfinished journal (status 5); compiler recovery takes the exclusive lock and
restores the last committed selector/export before retrying. A committed marker
allows finishing cleanup. fsync files/directories at the commit boundary; on any
parse/export/validation/write/rename failure retain or restore the prior valid
generation and public export, never expose a partial file. Native readers always
use the pinned generation, not the compatibility copy. Explicit legacy exports
also serialize with that lock and never overwrite native policy.bin/selector.

## Rollout and bounded import

`JARO_FILTER_ENGINE=legacy|native` selects both compilation and batch dispatch.
Unset means native; an empty/unknown value is an error. Missing native helpers, bad caches,
unqualified regex export and partial batch failures remain nonzero. No implicit
fallback follows an export failure. Explicit legacy uses fresh legacy cache
only; typed/domain records must fail with migration diagnostics before calling
the old compiler, which otherwise silently ignores them. Switching engines never
applies a message twice. `jaro -n filter` renders classification only; native
explain uses the same decisions. Comparing engines is decision-only: compare
independent fixture decisions/retained legacy observations, or a legacy adapter
whose decisions have been separately verified; do not treat ambiguous legacy
dry-run proposal lines as one final decision per message.
Normal native apply workflows compile/verify once before loading a generation.
Read-only explain/`-n` workflows use in-memory `--check --report0`, require that
fingerprint to equal the selected generation, and otherwise return status 5
requesting an explicit update. They never publish a cache or modify mail/books.
Direct jaro-filter uses the selected immutable snapshot without compiling;
`--generation ID` deliberately pins a prior compatible snapshot for reproduction.

The existing `jaro filter [MAILDIR]` and `jaro fetch` dispatch native batches
when unset or with explicit `JARO_FILTER_ENGINE=native`. Filter defaults to
`$JAROMAILDIR/incoming`; fetch uses that absolute path regardless of cwd.
Each native apply workflow invokes the compiler once, then passes its generation
ID to one batch invocation. Filter does not index. Fetch indexes once after a
batch attempt, including partial failure because intact messages may have moved;
it preserves the batch's failure if indexing also fails. Missing helpers,
transport failure and compilation failure stop before a batch/index attempt.
Notmuch setup can write its configuration without refreshing the index first.
`jaro -n fetch` explains the local incoming batch without transport, keyring,
publication or indexing. These native filtering commands require an existing
policy/root; bootstrap never creates default inputs for them, `sieve`, `update`
or `sieve-import`. Native `-n sieve` and `-n update` validate without publishing
or writing mblaze setup. Normal explicit `init` remains the initialization path.

`jaro filter --explain [MAILDIR]` and `jaro -n filter [MAILDIR]` render the same
native classification records. All selected bytes and the complete helper exit
status are checked before successful human records are rendered. Human output
quotes raw paths with Zsh ANSI quoting under the C locale; it is display text,
never evaluated as shell. Native global/message failures propagate without
fallback. A malformed stream with zero helper status fails with status 2;
otherwise the helper's nonzero status is retained. Summaries include that status.

`jaro filter --compare OBSERVATIONS [MAILDIR]` is also native and read-only.
It compares retained **actual final legacy observations**, not legacy dry-run
proposal lines. No production legacy apply or guessed matcher runs. General
automatic legacy classification is unsupported because its dry-run proposals
are ambiguous; supplying those proposals fails clearly. The observation producer
must retain provenance and exact message hashes for the unchanged synthetic or
archived corpus it observed. The CLI checks the policy fingerprint and complete
path/order/cardinality, but does not authenticate the producer or hash message
bodies. A caller must not reuse observations after changing that corpus.

The bounded observation file is `JLC1 NUL native-fingerprint NUL`, followed by
three NUL-terminated fields per occurrence: raw original path, final relative
local legacy Maildir destination, and literal `observed`. Native destinations
are also displayed relative to the resolved mail root. Its rows must cover the
selected byte-sorted batch in order. Empty destinations, ambiguous rows, wrong
fingerprints, native errors, mismatched paths and incomplete framing fail with
status 4; unreadable files return 3. Differences are reported as observations,
with status 0 when both decision sets are complete. They do not authorize moves
or assert equivalence. The regression producer actually applies legacy only to
disposable copies, checks all 23 retained baseline winners and complete body
bytes, then verifies the separately authored native targets and no mutation.

Generated v1 Sieve is an optional require declaration, seven top-level
`if true { STAGE-RULES }` blocks in fixed policy stage order, then one fallback
fileinto. Empty stages retain empty blocks. Each rule is `if TEST { FILEINTO;
stop; }`; test forms are address :all :is (typed address), address :domain :is
(apex), address :domain :matches "*.domain" (subdomains), qualified address
:regex (legacy ERE), Sender header :contains mailman-bounce (bounce), or
X-Spam-Flag header :is YES (spam). Header lists/keys are quoted strings or lists;
From-stage fields are From/Sender, To/own are To/Cc, blacklist/known are From.
Comparators are always explicit. Namespace mappings invert without ambiguity;
reject colliding mappings for different logical destinations when import cannot
recover them. Rule IDs/comments are optional audit hints, never the only source
of meaning: the fixed stage blocks and actual tests determine rule kind/order.
Exact-address tests always compare a complete mailbox using i;ascii-casemap;
import recognizes that predicate, ASCII-canonicalizes the key and never rewrites
it as separate local/domain conditions. A different comparator is unsupported.

The importer tokenizes strings, lists, comments and braces; it does not grep
line layouts. Accept quoted strings with Sieve quote/backslash escaping, LF/CRLF,
arbitrary insignificant whitespace and line/block comments. Reject encoded-NUL,
multiline text literals, include/redirect/discard/keep, envelope/body tests,
variables/foreverypart, unsupported comparators/extensions, arbitrary condition
trees and truncated/extra commands. This is a bounded generated grammar.
Parse/validate the entire script/profile/mappings before staging any writes.
`--check` reports unsupported constructs without changing rules/books/aliases.

Native import defaults to v1. `--legacy-generated` explicitly recovers the
previous generated header-contains layout covered by test-sieve-import.sh:
From blacklist groups -> blacklist book; From/Sender INBOX groups -> whitelist;
To/Cc priv groups -> aliases excluding configured account addresses; inline
To/Cc or From/Sender routes -> legacy to/from move records; fixed bounce/spam/
fallback blocks -> builtins. Reject other tests/actions, unknown sections or
ambiguous forms before writes. Recovery reconstructs original local rule text;
it does not claim the older header-substring script is semantically equivalent
to parsed-address/ERE policy v1. Emit that migration diagnostic. Under explicit
legacy dispatch the old import behavior remains until its replacement milestone.
Legacy recovery accepts omitted whitelisted blocks, restoring policy-v1 builtins
and emitting the migration diagnostic. An unused legacy require "variables" is
accepted only in this recovery grammar; variable commands remain unsupported.
Recovered legacy ERE routes still need a qualified regex export profile before
their merged generation can be published; recovery syntax alone is not an export
capability grant.

Import merges exact duplicate entries idempotently, preserving unrelated source
rules and accounts; unsupported collisions fail instead of rewriting unrelated
configuration. Stage all changed Filters.txt/books/aliases and a validated new
generation, then publish under the same recoverable lock/journal boundary.
On failed import restore every original input and previous generation/export;
there must be no successful partial import or remote deployment.

Native `jaro sieve-import FILE` now invokes this bounded importer;
`jaro -n sieve-import FILE` validates the complete prospective generation without
creating a cache or changing inputs. `--legacy-generated` selects only the
documented old generated grammar and emits `W_LEGACY_SIEVE`; it does not grant
regex export capability. Unset/explicit legacy dispatch retains the legacy
importer. Legacy dry-run is rejected because that importer cannot stage safely.

Stage position determines contacts, own addresses and builtins. Explicit route
mailboxes must have a unique inverse under the selected profile: a declared
mapping that also names a distinct destination in the namespace is ambiguous.
For example, with an empty prefix an explicit route to `INBOX` could mean named
`INBOX` or logical `known`; import rejects it. A prefix separating named mailboxes
from explicit mappings avoids that ambiguity. A non-slash namespace separator
occurring in an unmapped mailbox is likewise ambiguous between a literal
component and a path boundary; provide an explicit mapping. Builtin-stage
mailboxes are checked against their stage's configured mapping directly.

Existing equivalent selectors are retained, including their source bytes and
provenance. New rules append in imported source order; an existing selector with
a different destination, a reversed existing/imported order, duplicate script
predicates or a new rule that would need insertion before an existing imported
rule fails closed. Unrelated rules keep their original priority. Contacts append
fresh numeric abook records without rewriting existing names/comments; imported
contacts use the name `Imported Sieve`. Own addresses already supplied by accounts
remain in those accounts rather than becoming aliases. Original import files are
never moved or deleted. Mutable input symlinks are refused by import.

Imports take the exclusive publication lock, validate snapshots with prospective
input bytes in memory, stage/verify every product, and recheck the entire original
inventory after staging. `import.journal` durably records the four fixed input
paths' presence/modes before replacement. Its backups and the ordinary generation
journal share one commit marker. Recovery restores all uncommitted inputs plus
the prior selector/export, or retains all committed views. Readers and read-only
checks reject an unfinished input journal; the next mutating compiler invocation
recovers under the lock. SIGKILL may retain an owner's private `.building-*`
directory; recovery never sweeps unrelated staging directories. These checks do
not claim power-loss testing or isolation from arbitrary writers ignoring locks.
