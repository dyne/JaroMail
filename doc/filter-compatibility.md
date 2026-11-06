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
batch emits a zero-record success stream. Native is the CLI default;
explicit `JARO_FILTER_ENGINE=legacy` retains the rollback workflow.
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

The application default is **native**. Explicit `JARO_FILTER_ENGINE=native`
selects the same workflow; `JARO_FILTER_ENGINE=legacy` selects rollback.
Work on an existing initialized mail root and retain its original
policy/contact inputs before migration. `JAROMAILDIR` selects that root;
`JAROWORKDIR` selects the installed runtime. Direct C helpers always require
`--root` and do not infer it from cwd or HOME. Installed tests exercise detached
working directories and paths containing spaces.

Fresh `jaro init` chooses `Filters.native.txt` for unset/explicit native: no
explicit routes are active, and typed address/domain examples are commented.
Blacklist, bounce, known, spam, own-address and unsorted builtins remain active.
Explicit legacy initialization keeps the original `Filters.txt` with 14 ERE
rules. Selector and needed-template validation precedes bootstrap writes.
Every existing policy is retained byte for byte; initialization does not add
a regex qualification profile or guess a translation. Thus an existing legacy
policy can still produce the deliberate `E_REGEX_EXPORT` migration diagnostic.

An absent `FilterProfile.txt` selects `core-v1`, which exports typed exact and
domain predicates. Existing `from`/`to` POSIX ERE records need the explicitly
qualified `pigeonhole-2.4.5-posix-ere-v1` export profile or a deliberate typed-rule
rewrite. An unsupported/unqualified ERE reports status 4 with source-line
migration diagnostics and preserves the preceding generation/export. The
trusted compiler-version-bound catalogue qualifies only its declared subset;
a caller-supplied hash cannot add regex equivalence. See the complete
[profile and qualification contract](filter-interfaces.md#commands-and-statuses).

For example, these are policy-file records, not shell commands:

```text
from-address sender@example.org move Senders
from-domain example.org move DomainApex
from-domain *.example.org move DomainDescendants
```

The apex rule matches that domain itself; the wildcard requires a proper
descendant, including deeper subdomains. Both test parsed mailbox domains.
Typed address rules use whole-mailbox ASCII case folding, and source order is
deterministic. These are intentional corrections relative to the legacy table.

After choosing the policy/profile, publish explicitly, then inspect without
applying messages (replace `root` with the existing root):

```sh
root=/absolute/existing/mail-root
JAROMAILDIR="$root" JARO_FILTER_ENGINE=native jaro sieve
JAROMAILDIR="$root" JARO_FILTER_ENGINE=native jaro -n filter "$root/incoming"
JAROMAILDIR="$root" JARO_FILTER_ENGINE=native jaro filter --explain "$root/incoming"
```

Read-only dry/explain commands require an up-to-date selected generation;
changed valid inputs request an explicit update with status 5. They do not publish or create
default inputs. `--compare OBSERVATIONS` accepts complete retained actual legacy
observations for an unchanged corpus under the documented
[observation contract](filter-interfaces.md#rollout-and-bounded-import), including
its provenance/body-retention obligations. Ambiguous legacy dry-run proposals
are rejected. No legacy apply runs automatically during comparison.

To choose legacy for a subsequent workflow, set `JARO_FILTER_ENGINE=legacy`.
Keep legacy-compatible policy inputs for that choice: typed/domain records are
diagnosed before the old compiler can silently ignore them. Native and legacy
cache formats remain separate; a failed native compile, missing helper, corrupt
cache or partial batch never silently switches engines. Existing approved
blacklist/bounce/From/known/spam/own-address corrections remain visible in the
comparison table and independent targets.

### Tested build and assurance scope

The pre-rollout L7.2 checkpoint on 2026-11-06 used GNU/Linux x86_64, kernel
`6.12.113+deb13-amd64`, glibc 2.41, GCC 14.2.0, GNU Make 4.4.1 and Zsh 5.9.
`make test`, normal `make test-strict`, and native-scoped ASan/UBSan
`make test-strict` each executed all 36 discovered scripts and 215 native named
tests with zero failures/skips. The sanitizer claim covers the selected native
sources, production batch/worker and installed workflow helpers; ordinary
legacy/oracle builds are outside a global instrumentation claim. Current worker
TSan evidence covers 78 storage/batch tests, including cancellation, hooks and
dependencies. Controlled child operations are settled and source/no-change
snapshots remain checked.

Ordinary GNU build/install, the explicit native `-j4` graph, header dependencies,
unchanged builds, clean targets, space-containing PREFIX/DESTDIR, and both real
installed selectors passed. The two install locations each compiled, dry-ran,
applied and indexed the unchanged authored eight-message fixture under both
selectors: 32 installed-message results, exact IDs/destinations/body bytes and
index counts. Local static glibc inputs also supported a native-helper static
link and eight-message smoke. That check does not establish a self-contained
static application. Musl and other OS build/runtime paths were not executed.
The normal installed native binaries remain byte-identical to the performance
candidate; these documentation updates do not invalidate its measurements.

The pinned source-built Dovecot/Pigeonhole 2.4.5 frontends were freshly built
and then reused through verified cache checks. Minimal vendor scope, each
upstream byte digest, modified/missing/extra/symlink rejection and pristine
vendor bytes/modes passed. Oracle outputs stay outside upstream sources and
are not installed as application helpers. No Dovecot daemon or live mail service
is needed. Server/provider capabilities are explicit profile declarations;
these local simulations do not establish deployment compatibility with an
unqueried provider. The qualified regex/SMTPUTF8 scope remains the matched
PCRE2-32 plus experimental-mail-UTF8 build documented in
[sieve-tools.md](sieve-tools.md).

Complete local commands, environments, selected identities, process summaries,
source snapshots and artifact hashes for this checkpoint are retained under
`.gestalt/evidence/native-filter-execution/l7/l72/` in the planning workspace.
The native-default rollout checks and row-by-row compatibility audit are retained
under `.gestalt/evidence/native-filter-execution/l7/l73/`. Whole-branch acceptance
and terminal review remain separate from these local checks.

The current native-default refresh repeats `make test`, normal strict and
native-scoped ASAN/UBSAN strict: each passes 36 scripts and 223 native named tests,
zero failures/skips. Worker TSan's 80 storage/batch tests remain current by exact
C/header/build/thread-test identity, including forced pending/consumed signal
schedules; ordinary legacy/oracle binaries are outside global instrumentation.
Each workflow suite additionally exercises nine source/PREFIX/DESTDIR fresh-init
flows across unset/native/legacy, with 27 authored message outcomes, exact
template bytes, no-write selector/template failures and retained legacy-policy
migration diagnostics. Current packaging passes 38 checks, 48 installed selector
outcomes and the eight-message static glibc helper smoke. Other OS/musl/provider
execution remains unverified; the OSX packaging template copy is a source change,
not an executed OSX qualification. All 22 compatibility rows have current
evidence with unchanged independent golden files. Full commands and hashes are
under `.gestalt/evidence/native-filter-execution/l7/l73/release/`.
