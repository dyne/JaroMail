# Filter policy v1

This is the approved replacement policy, independent of legacy implementation
observations. `extras/test/fixtures/filter-policy/target.json` is the authored
decision corpus: 23 address/stage cases, 11 domain cases and four isolated
domain/order cases. Each has an immutable fixture ID, expected winning source
identity, local destination and Sieve mailbox. The legacy implementation does
not yet implement this contract. Its separate observations are in
`legacy.json` and [filter-compatibility.md](filter-compatibility.md).

Evaluate stages in this order, stopping at the first matching rule:

1. Blacklist: parsed **From** address -> `zz.blacklist`.
2. Bounce: **Sender** header contains `mailman-bounce` -> `zz.bounces`.
3. Explicit From routes: parsed **From and Sender** addresses.
4. Explicit To routes: parsed **To and Cc** addresses.
5. Known addresses and known domains: parsed **From** addresses -> `INBOX`.
6. Spam: **X-Spam-Flag** equals `YES`, ASCII case-insensitive -> `Spam`.
7. Own account/alias addresses: parsed **To and Cc** -> `priv`.
8. Fallback -> `unsorted`.

Address predicates examine extracted mailboxes, never display names, comments
or body text. Bounce intentionally retains the existing generated Sieve's
Sender substring condition. It does not become a From substring test.
Blacklist wins even when the address is also known and an explicit route or
spam test matches. Explicit routes win over known domains. From routes win
over To routes. Known senders win over spam. Spam wins over own recipients.

Within each stage retain original file order. For identical legacy
selector/pattern pairs retain only the last definition, including its source
position, before evaluating distinct rules in source order. `move` and `save`
mean the same move action. Any other action is an error with a source line.
Associative-map iteration and the sorted order of recipient addresses must not
choose a winner. Exact-address and domain routes share their corresponding
From/To stage; neither automatically outranks the other. First source position
wins their overlap. The sample corpus uses `filters:N` and `domain:N` to name
source lines in its respective rule arrays, plus named builtins/contact IDs;
these fixture identities are independent of future serialized cache IDs.

The supported Filters.txt records are:

```text
from legacy-posix-ere move folder
to legacy-posix-ere save folder
from-address person@example.org move folder
from-glob *@*.example.org move vendors
to-glob *+shop@*.ch move shopping
to-address team@example.org move folder
known-domain example.org
known-domain *.example.org
from-domain vendor.example move vendors
to-domain project.example move projects
```

The glob selectors match one complete parsed address, using ASCII
case-insensitive comparison. `to-glob` examines To and Cc, and `from-glob`
examines From and Sender. A star matches zero or more characters, including
dots. Plus, dot, question mark and square brackets match themselves.
In an unquoted pattern, `\*` matches a literal star and `\\` matches
a literal backslash. Inside a JSON-quoted token, double each backslash
again. Other glob escapes produce a line diagnostic.

For the shopping rule, `lea+shop@example.ch`, `a+b+shop@mx.example.ch` and
`+shop@example.ch` select `shopping`. With empty addressbooks and the default
policy, `lea@example.ch`, `lea+shopping@example.ch` and
`lea+shop@example.ch.example.org` fall through to `unsorted`. A To
address of `lea+shop@example.org` and a Cc address of `bob@example.ch`
also fall through. Pigeonhole evaluates each address separately.

Legacy four-field `from`/`to` patterns use POSIX ERE under clean Zsh semantics
(`RE_MATCH_PCRE` off); compile each once. Regex punctuation retains its regex
meaning. In particular `github.com` is a regex with an unescaped dot, not a
typed domain. Shipped `docs/Filters.txt` contains 14 active From/save records
routing to `zz.social`; preserving those records does not make their existing
regex semantics exact domain matching. No private representative rules were
provided; the public synthetic corpus represents address conflicts instead.

Domain matching is ASCII case-insensitive and label-aware. `example.org` matches
only that apex; `*.example.org` matches one or more complete subdomain labels,
including deep descendants, excluding the apex. Use both records for both.
Neither matches `badexample.org` or `example.org.attacker.net`. Only ASCII
domains or ASCII A-labels are accepted initially. Reject non-ASCII, empty labels,
leading/trailing label hyphens, trailing dots, invalid characters and general
glob patterns with line diagnostics. Labels are 1–63 ASCII letters/digits/hyphens
with alphanumeric ends; the complete domain is at most 253 bytes. Wildcard `*.`
is allowed only at the beginning and does not form part of that length. Domain
input is not partially IDNA-converted. Do not put wildcard domains in abook.

Typed exact-address, known, blacklist and own-address predicates compare the
whole parsed mailbox ASCII case-insensitively. Canonicalize ASCII A-Z to a-z in
both local-part and domain; preserve other bytes without Unicode case folding.
Before case mapping, `mailbox-minimal-quoting-v2` decodes a quoted local part
and its quoted pairs, then serializes it with minimum necessary quoting.
Dot-atoms lose redundant quotes/escapes; spaces, embedded quotes/backslashes,
leading/trailing dots and repeated dots retain quotes, with quote/backslash
re-escaped. Empty local parts and invalid/control bytes are unsupported.
Typed predicates, contacts/own inputs, raw-header parsing and direct parsed
mailbox inputs all use this operation. ERE pattern bytes remain unchanged and
match the canonical mailbox. Supported UTF8 local bytes remain unchanged;
domains retain the ASCII restrictions above. This corrects the earlier native
assumption that redundant quotes/escapes were meaningful mailbox bytes.
It follows minimum-quoting equivalence in
[RFC 5321 section 4.1.2](https://datatracker.ietf.org/doc/html/rfc5321#section-4.1.2)
and pinned Pigeonhole's `smtp_address_encode` serialization. The retired shell engine is represented by historical observations.
An exact address is not an address glob. This refines the earlier draft's
local-part-sensitive choice so core Sieve can use one equivalent whole-address
predicate: `address :all :is :comparator "i;ascii-casemap"`. Never combine
independent localpart/domain tests: they can pair different addresses in a
multi-address header. The comparator applies to the selected string, as specified
by [RFC 5228 sections 2.7.3 and 5.1](https://www.rfc-editor.org/rfc/rfc5228.html#section-5.1).

Whole-mailbox folding explicitly corrects local own-address casing behavior;
legacy uppercase recipient probes are characterized separately. Legacy ERE
matching remains POSIX `REG_EXTENDED|REG_ICASE` under LC_ALL=C, matching the
shell's CASE_MATCH-off baseline. Domain grammar still folds ASCII labels only.
`extras/test/fixtures/filter-policy/casing.json` adds 14 independently authored target
decisions for casing and cross-address negatives; these are future native/Sieve
expectations, not claims that legacy implements them.

Logical `known` maps to `INBOX` and logical spam to `Spam` by default on both
local and server delivery. Explicit mailbox mappings apply to both. The
fixture's `local` field records the pre-cutover target. Empty rule/contact sets
still retain builtin stages and fallback. An empty Maildir is a successful no-op.
Empty/malformed messages must be reported safely and retained recoverably;
legacy empty-file deletion is an intended correction, not target behavior.
Message content and authentication policy must not change.

Core Sieve profiles export typed address/domain records. Legacy ERE export
requires an explicitly declared regex-capable profile and demonstrated semantic
equivalence using the actual pinned Pigeonhole 2.4.5 oracle. A substring or glob
translation is not an equivalent regex export. Unsupported constructs fail
with rule/source-line migration diagnostics and leave the previous generation
usable; never omit or weaken a rule. Capabilities and mailbox mappings are
configurable. No remote provider compatibility is claimed.

The portable policy is `Filters.sieve`. The local publisher binds the script
and its bytecode to the inputs, runtime and profile. A server must compile
the source with its own matching capabilities. The LMTP qualification uses
Pigeonhole 2.4.5 with the same fileinto and mailbox extensions. When the
server fixture disables mailbox, its script fails validation and the server
performs an implicit keep in INBOX. A successful LMTP delivery reply can
therefore accompany an unapplied filter. Check the server's Sieve log.

The parity suite exercises the authored winners using the installed local
frontend and the pinned independent `sieve-test` oracle. It repeats the cases
after bounded import and includes wrong-destination and wrong-priority
mutation controls. Physical delivery tests compare occurrence paths, body
digests and modes against independent inventories. Set `JARO_FILTER_PARITY_WORK`
to retain the parity matrix, command logs, oracle inputs and traces.
