# Legacy filter compatibility baseline

Native compiler v2 corrects a representation assumption from the initial shared
core: equivalent quoted locals now use minimal quoting before matching and
identity generation. Necessary quotes/escapes survive. Thus the anchored
benchmark pattern matches `"from0000"@example.org`, and `github.com` matches
`"git\hub.com"@example.org`, as the pinned Sieve parser does. Regex patterns
themselves are unchanged; one newly authored probe whose pattern explicitly
requires redundant quotes now correctly misses. The original 52 target winners
are unchanged. The independent normalization fixture records the equivalent
spellings, including UTF8 locals under the qualified SMTPUTF8 build. This is a
narrow native policy correction; the retained legacy shell engine and recorded
baseline observations are unchanged.

Native classify and apply regressions now exercise all original 52 authored
target winners, stable IDs and provenance against actual immutable generations,
and the same generation's Sieve against the pinned regex/SMTPUTF8 oracle. The
table below remains a legacy baseline: native blacklist, bounce, From/Sender,
spam, own-casing and domain results follow its explicitly approved corrections.
Native empty or malformed messages remain in place with status 7, and an empty
batch emits a zero-record success stream. Legacy remains the CLI default;
explicit `JARO_FILTER_ENGINE=native` enables native filter/fetch application.
Native `-n`/`--explain` require an up-to-date selected generation and never
publish. `--compare FILE` compares complete retained actual legacy observations
without applying either engine. It rejects ambiguous proposal text; see the
[CLI and observation contract](filter-interfaces.md#rollout-and-bounded-import).

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
| 14 shipped From/save defaults | github sender falls through to unsorted | all 14 emit zz.social routes | Correct From extraction; keep documented legacy ERE semantics |
| Empty Filters.txt, fresh root | unknown sender -> unsorted | no explicit routes; builtins retained | Preserve fallback |
| Empty file | deleted, command exit 0 | no executed Sieve claim | Correction: retain recoverably and diagnose |
| Empty Maildir | exit 0 (legacy summary counts an empty line as one mail) | not applicable | Preserve success, correct count to zero |
| 15 domain/order target cases | unsupported legacy grammar, not submitted to legacy compiler | not generated | Add typed rules; execute targets in later milestones |

The shell's From extraction invokes `mseq -c` then `maddr -h from -a` without
a message path on stdin. To/Cc extraction supplies the path. The observed
local From, known and blacklist fallthroughs are defects, not portable policy.
Local code has no independent Sender bounce stage and tests blacklist after
spam. `mpick` status does not reliably mean that its filtered stdout selected a
message: current fixtures with YES still fall through. These mechanisms explain
the observations without establishing behavior on every mblaze version.

Generated Sieve uses raw `header :contains`; legacy local regexes use POSIX
ERE. This is an acknowledged semantic mismatch even when the destination is
the same. The characterization asserts actual emission, stage positions,
duplicate collapse and presence of overlapping routes. It does **not** execute
an invented Sieve evaluator or claim semantic Sieve results. Independent pinned
oracle execution and regex capability qualification belong to later milestones.

A sequential same-second probe changing defaults to an empty Filters.txt
retained `zz.social` in generated output in the first characterization run.
The fresh-root empty-rules test avoids conflating a cold policy case with
legacy compiled-cache freshness. This is a cache-publication hazard to verify
and correct in the native generation milestone, not a promise of stale output
or an assertion that every rebuild fails.

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
