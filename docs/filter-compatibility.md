# Legacy filter compatibility baseline

The table records the former shell engine's clean-Zsh/mblaze observations.
The independent target fixtures define the corrected Sieve behavior. Current
local filtering executes the same generated rules through Pigeonhole. Use
`jaro filter --compare` with retained, final observations to compare with
historical deliveries without changing mail.

Baseline application revision: `165e61e`; preparation revision `7161a2e` changes
only the minimal vendor extraction. Observations below use the real `jaro`
CLI, clean Zsh with POSIX ERE (`RE_MATCH_PCRE` off), and mblaze. The independent
target is [policy v1](filter-policy.md). `target.json` must never be regenerated
from this table or from a backend's output.

| Case / fixture IDs | Observed local result | Generated Sieve structure | Target classification |
| --- | --- | --- | --- |
| `blacklist` | unsorted | From header contains blacklist | Correction: parsed From -> zz.blacklist first |
| `blacklist-known-spam-route` | tobucket | blacklist precedes all routes | Correction: blacklist wins -> zz.blacklist |
| `bounce`, `bounce-before-to` | unsorted, tobucket | Sender contains mailman-bounce before routes | Correction locally: bounce -> zz.bounces |
| `from`, `sender` | unsorted | From/Sender header contains pattern | Correction locally: parsed From/Sender -> frombucket |
| `from-before-to` | tobucket | To emitted before From | Correction both: From wins -> frombucket |
| `to`, `cc`, `to-before-known` | tobucket | To/Cc header contains pattern | Preserve stage result; correct parsing/regex semantics |
| `duplicate-last` | lastbucket | only lastbucket emitted | Preserve last identical selector/pattern definition |
| `overlap-first` | firstbucket on studied Zsh | both overlapping rules emitted in map order | Make source-order result deterministic; no map-order guarantee |
| `known`, `known-before-spam` | unsorted | From/Sender header contains known -> INBOX | Correction locally: parsed From -> known, before spam |
| `known-sender-only` | unsorted | Sender can match known -> INBOX | Preserve local fallback; narrow generated known test to From |
| `display-name-not-address` | unsorted | raw known substring also sees display text | Preserve local fallback; correct generated test to parsed addresses |
| `spam`, `spam-lowercase` | unsorted | header :is YES -> Spam | Correction locally: case-insensitive YES -> zz.spam |
| `spam-before-own` | priv | spam before own -> Spam | Correction locally: spam wins -> zz.spam |
| `own-to`, `own-cc` | priv | To/Cc contains own address | Preserve destination; correct substring matching to parsed address |
| Uppercase own To/Cc probes | unsorted | default ASCII-casemap comparison | Correction locally: ASCII-fold whole mailbox -> priv |
| Typed exact/known/blacklist casing and cross-address cases | new typed rules; see separate target corpus | explicit address :all :is i;ascii-casemap | One whole-mailbox predicate, never independent localpart/domain pairings |
| `fallback`, `missing-fields` | unsorted | final fileinto unsorted | Preserve fallback |
| 14 retained legacy From/save defaults | github sender falls through to unsorted | all 14 emit zz.social routes | Correct From extraction; keep documented legacy ERE semantics |
| Empty Filters.txt, fresh root | unknown sender -> unsorted | no explicit routes; builtins retained | Preserve fallback |
| Empty file | deleted, command exit 0 | no executed Sieve claim | Correction: retain recoverably and diagnose |
| Empty Maildir | exit 0 (legacy summary counts an empty line as one mail) | not applicable | Preserve success, correct count to zero |
| 15 domain/order target cases | unsupported legacy grammar, not submitted to legacy compiler | not generated | Typed rules implemented and executed by the native/pinned-Sieve target corpus |

The shell's From extraction invokes `mseq -c` then `maddr -h from -a` without
a message path on stdin. To/Cc extraction supplies the path. The observed
local From, known and blacklist fallthroughs are defects, not portable policy.
Local code has no independent Sender bounce stage and tests blacklist after
spam. `mpick` status does not reliably mean that its filtered stdout selected a
message: current fixtures with YES still fall through. These mechanisms explain
the observations without establishing behavior on every mblaze version.

Legacy-generated Sieve uses raw `header :contains`; legacy local regexes use POSIX
ERE. This is an acknowledged semantic mismatch even when the destination is
the same. The characterization asserts actual emission, stage positions,
duplicate collapse and presence of overlapping routes. It does **not** execute
an invented Sieve evaluator or claim semantic Sieve results. Independent pinned
oracle execution and regex capability qualification now have separate native
parity/qualification suites; they do not turn the historical legacy structural
observations into an assertion of legacy semantic parity.

A sequential same-second probe changing defaults to an empty Filters.txt
retained `zz.social` in generated output in the first characterization run.
The fresh-root empty-rules test avoids conflating a cold policy case with
legacy compiled-cache freshness. Native compilation now snapshots and hashes
complete inputs and selects immutable generations, with repeated/same-second
edit, recovery, corruption and publication-pinning regressions. The observation
remains a legacy cache-publication hazard, not a native freshness promise or an
assertion that every legacy rebuild fails.

Run `zsh extras/test/test-filter-policy.sh` with the normal strict-test
dependencies. It stages the actual CLI using existing test helpers, checks
23 named messages, two uppercase own-recipient probes, and defaults/empty-rules
messages, alternates cur/new,
asserts exact IDs and destinations, success exits, byte hashes, duplicate-free
accounting, empty-file deletion and empty-Maildir status. Target-only domain
records are counted and validated separately (38 total authored decisions).
Another 14 independently authored casing/cross-address targets are schema-checked
by test-filter-contract.sh, for 52 original target decisions overall. The
`test-filter-parity.sh` matrix now executes all 52 against authored winners, pure
native classification, immutable batch classification and pinned Sieve, before
and after bounded import. It additionally executes 17 mapping/escaping targets,
empty-policy/no-create cases and the qualified regex corpus, with wrong-destination
and wrong-priority mutation controls. The expanded `test-filtering.sh` retains
the legacy observations and separately asserts nine actual native apply results;
sender/blacklist/known/private cases must reach their named destinations with
unchanged bodies. The earlier local-part-sensitive draft was replaced before
implementation by explicit whole-mailbox ASCII folding.
The existing filtering regression remains intact. Full suite: `make test-strict`.
Tests use only disposable directories and synthetic messages; no services,
provider credentials, live mail or keyrings are needed.


## Recovering abbreviated addressbook contacts

Older `fetchaddr` versions shortened display names longer than 30 bytes to
27 bytes plus `...`. Learning via Mutt's `A` binding then passed the name and
mailbox through abook's header importer. With an email-shaped display name,
that importer can select the display name instead of the actual mailbox and
store the abbreviated address. The importer also has fixed field-length limits.
The corrected learning path preserves full mailbox/name pairs and appends
native records without this importer; abook does not need to be vendored.

Back up the books before attempting recovery. For example:

```sh
root=/home/jrml/Mail/jaromil
backup=$(mktemp -d "$root/addressbook-recovery.XXXXXXXX")
cp -p -- "$root/"*.abook "$backup/"
grep -n '^email=.*\.\.\.$' "$root/whitelist.abook"
```

The grep finds a common damage signature, not every possible invalid address.
After installing the corrected build, reselect original messages in Mutt and
press `A`, or learn from a saved original message:

```sh
JAROMAILDIR="$root" jaro -l whitelist learn all < /path/to/original.eml
```

Original message headers may still contain the complete addresses. Relearning
adds complete contacts while preserving old records. Check the recovered
addresses against those headers before removing damaged entries through abook.
Finish interactive abook edits before learning/importing. Do not guess a suffix
from `...`: the missing bytes cannot be recovered from the damaged entry alone.
If no original message or older backup remains, recovery may be impossible.

Filter compilation warns and omits invalid exported contacts while keeping all
valid contacts and the original books. Warnings report exported address rows,
not native file line numbers. A failing exporter or malformed book structure
still stops compilation; it is not silently treated as an empty addressbook.

## Migration and installed-workflow qualification

Pigeonhole executes local filters. The default and the `sieve`, `native` and
`legacy` values of `JARO_FILTER_ENGINE` select the same installed runtime.
Run `jaro fresh-filters` to migrate an older generation from your existing
`Filters.txt`, aliases, accounts and addressbooks. Until then, filter and fetch
report the required update before touching messages or starting transport.
An unsuccessful migration preserves the last coherent policy and all mail.
Historical messages stay in `known` and `zz.spam`. New known and spam deliveries
use `INBOX` and `Spam` by default. Custom Sieve mailbox mappings apply locally
and to the exported script. Existing files in the destination Maildir are preserved.

Use `jaro -n fresh-filters` to check authored inputs and `jaro fresh-filters` to publish.
`jaro sieve-import --legacy-generated FILE` recovers the bounded older
Jaromail export grammar. A failed import leaves the original policy, books
and public export in place. `jaro -n sieve-import FILE` previews an import.
The [interface contract](filter-interfaces.md) describes the supported policy,
publication and observation formats. See [Sieve tools](sieve-tools.md) for
installation and platform prerequisites.
