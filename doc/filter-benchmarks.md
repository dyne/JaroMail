# Reproducing filter benchmarks

Run from a dependency-equipped source checkout:

```sh
zsh extras/bench/filter-baseline.sh --output /absolute/empty/report-directory --runs 3
# Explicit native comparison, independent of the application selector default.
zsh extras/bench/filter-baseline.sh --engine native --output /absolute/empty/native-report-directory --runs 3
```

The entry point snapshots source before building/staging, so generated manuals
do not modify the checkout. It uses existing test helpers and a private install;
fixture creation, staging and verification are outside measured operations.
`--sizes 10 100 1000 --messages 100 1000` are the defaults. Each workload gets
three independent timing repetitions and three independent instrumentation
repetitions. The strict suite runs a legacy harness regression with 10
rules/contacts and eight messages, then native regressions with independently
varied 10/20 rules/contacts and 8/12 messages. Each has one repetition per mode.
These smoke runs verify decisions and bounded subprocess counts; they are not
timing baselines. Output must be empty to prevent evidence replacement.

The deterministic v1 generator writes half From and half To POSIX ERE rules,
with exactly the selected number of abook contacts. Each message batch contains
25% To matches, 25% misses, 25% From/To conflicts and 25% known-only senders.
Messages alternate cur/new and include stable IDs and byte-identical bodies.
`fixtures.json` preserves every input-file digest and aggregate input identity,
independent legacy and target destination tables, distribution and message
hashes. All repetitions verify those input digests. This is a synthetic public
workload; no private mailbox, server, keyring or provider credentials are used.

The legacy destination table deliberately reflects the separate compatibility
baseline: To matches and conflicts go to `route-0`/`route-1`; misses and known-only
senders fall through to unsorted because of the characterized From extraction
defect. Target conflicts instead select `from-routes`; target known-only senders
select `known`. Both tables are authored in the generator; neither is generated
from observed output. Baseline success requires every applied message ID,
destination and byte digest, with no missing or duplicate messages. A faster
run with different accounting is invalid.

| Phase | Measured operation and cache state |
| --- | --- |
| update_filters-cold | Actual `update_filters` function; no filter cache initially |
| update_filters-warm-unchanged | Same function again, same content and existing cache, new process |
| sieve-emission | Actual `update_sieve` function, existing filter cache |
| jaro-sieve-unchanged | Full actual `jaro sieve` CLI, existing outputs and unchanged inputs |
| classification-dry | Full actual `jaro -n filter incoming` CLI; no message writes |
| application | Full actual `jaro filter incoming` CLI; verifies all final messages |
| index-cold | Actual `jaro index` after application, no notmuch database initially |
| index-unchanged | Actual `jaro index` again, existing database and unchanged messages |

Cold refers to application cache/database state, **not** OS page-cache eviction.
No system cache flushing or unrelated service control occurs. Warm/unchanged
operations run in a new process with warm files and persistent application
outputs. The legacy compiler rebuilds even unchanged inputs; the later native
compiler must prove its own fast path on these same inputs.

`wall_seconds` measures the complete command, including bootstrap, real logging,
locking and helper processes. Direct-function phases additionally record
`stage_seconds` around the actual function body, after normal bootstrap/account/
mblaze setup. No fn/act/lock/addressbook stubs are used. Index measurements
include the existing CLI's config generation, scanning, tagging and compaction;
they are reported separately from compile/matcher/application costs. The harness
verifies notmuch's count after each index run. Verification commands are excluded
from timing and executable counts.

The legacy dry run continues after a route proposal and can also print fallback
for that same message. It is **not** a one-decision classifier API. The harness
asserts this known trace (half a batch of route proposals and a full batch of
fallback proposals), as well as unchanged relative message paths, destinations
and hashes. Do not mistake proposal totals for extra delivered messages. Later
native pure classification must produce exactly the authored target decisions;
the native performance comparison must retain this legacy dry-run limitation
and the approved policy corrections in its report.

`samples.json` records all commands/cwds, exits, UTC observations, wall/stage
times, user/system time and per-command `wait4` peak resident memory. Memory is
KiB on the recorded Linux host and includes command descendants; it is a peak,
not a sum of per-helper peaks. It also includes pre-exec launch memory: a control
`/bin/true` measurement rose from 11544 to 52820 KiB after allocating 40 MiB in
the Python launcher. Thus these inclusive observations have a launcher RSS
floor and cannot isolate smaller engine memory differences. No isolated engine
memory improvement is claimed. `environment.json` captures host identity,
tool versions/binary hashes, environment, source build-script digest and C helper
flags. `summary.json` contains medians and all instrumentation count samples.
Raw stdout/stderr, phase clocks and exec logs remain beside those manifests.
For index phases the recorded counters are snapshotted before the independent
`notmuch count` check; the raw exec log also includes that trailing verification
call. Count comparisons use the recorded snapshot, not that additional query.

Instrumentation runs use small transparent PATH wrappers that append only an
executable name and then `exec` the originally resolved executable with unchanged
arguments/status. This counts the declared selected external commands, including
bootstrap calls. It does not count shell forks, builtins or executables resolved
outside PATH. These counts are a bounded process-growth proxy, not a claim of
complete process-tree coverage. No strace/perf/time package is installed for this
work. Instrumented wall times are excluded from performance medians. Compile
profiles must actually observe at least one `cat` per rule; dry profiles must
observe at least two `maddr` calls per message, so an empty profiler cannot pass.

Release comparisons use the same generator/input hashes, host, dependency
identities, flags, counts and cache definitions. On the 1000-rule/1000-contact/
1000-message workload the targets remain at least 5x faster compilation and
3x faster classification, with no material small-workload slowdown. Report full
CLI sieve time as the compilation gate, function times to explain its cost,
and dry classification separately from application/indexing. Dedicated
uninstrumented repetitions supply wall-time gates; deterministic tests must also
prohibit per-rule/per-message subprocess growth. These legacy measurements do
not claim a speedup or satisfy the later native release gates.


## Recorded baseline

The 2026-11-05 run on `alpstein` (x86_64, Linux 6.12.113, 16 reported CPUs)
completed 288 checked phase samples: six workloads, eight phases, three timing
and three separate profile repetitions. The versioned aggregate data is
[`extras/bench/baseline-legacy.json`](../extras/bench/baseline-legacy.json).
Full logs/manifests retain individual samples and hashes; the JSON binds their
identities. No speedup is claimed at this baseline stage.

All values below are medians in seconds. Compile/emission columns show function
body time; full CLI columns include bootstrap and helpers.

| Rules/contacts | Messages | Compile cold | Compile unchanged | Emit | Full sieve CLI |
| --- | --- | --- | --- | --- | --- |
| 10 | 100 | 0.035 | 0.041 | 0.024 | 0.109 |
| 10 | 1000 | 0.040 | 0.041 | 0.024 | 0.107 |
| 100 | 100 | 0.114 | 0.120 | 0.028 | 0.188 |
| 100 | 1000 | 0.120 | 0.120 | 0.025 | 0.184 |
| 1000 | 100 | 0.948 | 0.939 | 0.094 | 1.079 |
| 1000 | 1000 | 0.907 | 0.964 | 0.094 | 1.122 |

| Rules/contacts | Messages | Dry classify CLI | Apply CLI | Index cold CLI | Index unchanged CLI |
| --- | --- | --- | --- | --- | --- |
| 10 | 100 | 1.155 | 1.190 | 0.225 | 0.116 |
| 10 | 1000 | 11.795 | 12.060 | 0.397 | 0.144 |
| 100 | 100 | 1.262 | 1.291 | 0.218 | 0.120 |
| 100 | 1000 | 12.136 | 12.651 | 0.353 | 0.143 |
| 1000 | 100 | 1.981 | 1.897 | 0.220 | 0.120 |
| 1000 | 1000 | 19.673 | 18.903 | 0.363 | 0.149 |

At 1000 rules/contacts/messages the selected-command profiles observe
1006 `cat` calls for cold array compilation and
2000 `maddr`, 1000 `mseq` and 1000 `mpick` calls for dry classification.
The inclusive wait4 median was 27364 KiB for that dry command; interpret it
with the launcher-floor limitation above. Per-phase memory, count medians,
repetition counts and wall ranges are in the JSON.

## Native comparison method

`--engine native` retains the v1 generator and every original fixture digest,
including the comment-only empty blacklist. It adds a separately recorded
`FilterProfile.txt` declaring the already qualified
`pigeonhole-2.4.5-posix-ere-v1` export capability. This is necessary because the
unchanged baseline rules use legacy ERE syntax. The profile hash and regex
qualification digest are recorded in `environment.json`; no message, rule,
contact, alias or account input is normalized for the native run.

Native dry runs must emit exactly one decision for each authored message ID,
with the target table's destination. Application checks every target
destination, content digest, survivor and duplicate; indexing must account for
the complete batch. Legacy results retain the characterized legacy table and
proposal trace. Native conflict winners and known-only senders therefore differ
from the legacy defects by design; equal destinations across those cases would
be a correctness failure.

In native mode, separate profile runs wrap both selected PATH executables and
the installed absolute `jaro-filter-compile`/`jaro-filter` helper calls. Timing
runs use the ordinary runtime without those wrappers. Every profile phase must
have the same selected executable counts across independently varied rules and
batch sizes. Classification and application must each invoke one compiler and
one batch helper, with no per-message `maddr`, `mseq` or `mpick` calls. This still
measures a declared executable subset, not every fork or syscall.

Unchanged-input phases verify the selected generation, its product bytes,
inodes and modification times, and the public Sieve export remain unchanged.
Recovery markers are excluded: a successful publication leaves its committed
journal for cleanup by the next writer. Cleaning that journal does not rebuild
or republish a policy product. The focused generation fast-path regression also
forces parsing to fail after publication and verifies an unchanged compile
still succeeds, which distinguishes reuse from a rebuild producing equal bytes.

Cold native compilation builds and durably publishes all policy products.
The legacy cold `update_filters` phase builds its Zsh arrays; Sieve emission is
separate. These phase costs describe different work, so the compilation speed
gate compares the complete `jaro sieve` CLI under the same unchanged-input cache
definition. Application and indexing remain separate measurements, including
any slowdown; classification speed cannot establish an application speedup.

The native storage path currently copies and verifies each message, publishes
without clobbering existing mail, and durably synchronizes its destination and
source recovery namespaces. Those operations have an application cost even
when both Maildirs are on the same filesystem. A benchmark must retain that
cost rather than bypassing storage safety or counting a dry run as application.

## Historical singleton measurements and unresolved application gate

This historical capture predates the bounded worker implementation below. Its
failed application gate motivated the measured tuning; the original values and
full artifacts are retained unchanged.

The 2026-11-06 comparison on the same `alpstein` host completed another 288
checked phase samples: six unchanged v1 workloads, three uninstrumented timing
runs and three separate profile runs per workload. All original fixture
manifests, host fields, runtime executable hashes and tool versions match the
accepted legacy baseline. Both use GCC 14.2.0 and normal GNU/Linux `-O3` helper
builds. A separate fresh `-O3` build produced byte-identical native executables
to those measured; build commands show compile-only ccache use. No sanitizer
timing is included.

On 1,000 rules/contacts/messages, full unchanged-input `jaro sieve` fell from
1.122251 s to 0.087170 s (**12.87×**), and dry classification fell from
19.672596 s to 0.260014 s (**75.66×**). These exceed the 5× and 3× targets.
However, the small-workload condition is **unresolved and does not pass as an
unqualified release gate**: applying 100 messages with 10 rules/contacts rose
from 1.190384 s to 5.727579 s (**4.81× slower**). Medium application rose from
18.902562 s to 59.940705 s (**3.17× slower**). The native cold `update_filters`
function also takes longer, but performs unequal work: native compilation builds
all policy products while the legacy function builds only shell arrays. Its
absolute overhead remains reported; the comparable compilation gate is the full
actual `jaro sieve` CLI. Supplemental cold CLI comparisons start both engines
without application filter-cache artifacts and include complete emission, with
unchanged original fixture bytes and independent message accounting afterward.
Index samples are reported with their ranges and absolute differences for
materiality assessment. These measurements do not authorize enabling the native
default.

The following native medians are seconds. Function columns measure the body
after normal bootstrap; CLI columns include bootstrap, locking and helpers.

| Rules/contacts | Messages | Compile cold | Compile unchanged | Emit | Full sieve CLI |
| --- | --- | --- | --- | --- | --- |
| 10 | 100 | 0.0940 | 0.0414 | 0.0111 | 0.0557 |
| 10 | 1000 | 0.0960 | 0.0290 | 0.0116 | 0.0552 |
| 100 | 100 | 0.1604 | 0.0537 | 0.0599 | 0.0974 |
| 100 | 1000 | 0.2794 | 0.0822 | 0.0400 | 0.0937 |
| 1000 | 100 | 0.1791 | 0.0727 | 0.0473 | 0.0861 |
| 1000 | 1000 | 0.1590 | 0.0707 | 0.0481 | 0.0872 |

| Rules/contacts | Messages | Dry classify CLI | Apply CLI | Index cold CLI | Index unchanged CLI |
| --- | --- | --- | --- | --- | --- |
| 10 | 100 | 0.0474 | 5.7276 | 0.2413 | 0.1276 |
| 10 | 1000 | 0.1692 | 57.0509 | 0.3822 | 0.1416 |
| 100 | 100 | 0.0532 | 8.5989 | 0.2998 | 0.1354 |
| 100 | 1000 | 0.1837 | 66.0829 | 0.5346 | 0.2086 |
| 1000 | 100 | 0.0942 | 5.4211 | 0.2627 | 0.1497 |
| 1000 | 1000 | 0.2600 | 59.9407 | 0.4027 | 0.1340 |

Native dry checks verified 19,800 exact target decisions across timing/profile
runs, and application verified the same number of complete messages with exact
IDs, destinations and body hashes. Every index phase counted the entire batch.
The medium target is 250 `from-routes`, 250 `known`, 125 each `route-0`/`route-1`,
and 250 `unsorted`; the legacy result deliberately retains its different
characterized decisions. All 252 unchanged-input product checks passed. The
separate focused fast-path test also passed with parsing failure injected after
publication, proving that unchanged compilation bypasses parsing.

All native profiles have identical selected executable counts per phase across
the six workloads. Cold compilation exports the nonempty addressbook once;
unchanged compilation does not invoke abook. Classification/application each
invoke one compiler and one batch helper, with no `maddr`, `mseq` or `mpick`
calls. Indexing still invokes notmuch seven times per workflow independently
of rule/message counts. These are bounded executable counts, not syscall counts.

The native medium dry/apply inclusive wait4 RSS medians were 30,252/29,368 KiB;
the legacy dry median was 27,364 KiB. The launcher's varying RSS floor described
above prevents attributing these differences to engine memory. No isolated
memory improvement is claimed.

A separate instrumented 100-message diagnostic preserved the original inputs
and all eight correctness checks. Application took 5.445 s, with 5.330 s spent
inside fsync calls (97.9%): the compiler made seven calls and the batch helper
made 1,302, all successful. This diagnoses synchronization cost on the measured
filesystem; it is excluded from every wall-time gate. No durability operations
were removed to obtain the reported speedups.

The complete local captures are retained under
`.gestalt/evidence/native-filter-execution/l7/native-full-1/` in the planning
workspace; `comparison.json` binds the original/native manifests, all phase
medians, memory observations and selected process counts. Its SHA-256 is
`d2cb48c6a64ce54b5a6f359582ffee7765d2e00d2118b00c7438b3026502126c`.
`focused-manifest.json` binds the 80-phase harness regression, fast-path test,
optimized-build identity and separate fsync diagnostic. Reproduce captures
using the commands above; any claim that the complete rollout performance gate
passes needs supported tuning or an explicit reviewed plan revision first.


## Final bounded-worker comparative qualification (2026-11-06)

The final candidate completed **288 native phase checks**: all six original
workloads, three dedicated uninstrumented timing repetitions and three separate
profile repetitions. Fresh current-legacy controls completed **144 timing
checks** on the same six workloads. Every phase passed with zero failures or
skips, identical original input manifests, host, runtime/tool identities,
normal GNU/Linux `-O3` builds and application-cache definitions. Tests, builds
and separate resource diagnostics did not overlap these timing runs. The
native compiler binary is byte-identical to the preceding accepted compiler;
the worker change retains every singleton copy, verification, no-clobber,
private-claim and durability barrier.

On 1,000 rules/contacts/messages, full unchanged-input `jaro sieve` is
**0.087269 s**, versus the accepted baseline's **1.122251 s (12.86×)**.
Dry classification is **0.253856 s**, versus **19.672596 s (77.50×)**.
Both exceed the unchanged 5×/3× gates. Against fresh current-legacy controls the
corresponding ratios are 22.89×/80.34×; the acceptance comparison remains the
original baseline, so slower current controls do not inflate the stated gates.
All 252 unchanged-product checks passed. A refreshed real-compiler test also
passed with parsing failure injected after publication, confirming that
unchanged compilation bypasses parsing rather than rebuilding identical bytes.
The native dry and application checks each verified 19,800 messages across
wall/profile runs, with exact target decisions, IDs, destinations and bodies.

The following final native medians are seconds. Function-body columns retain
the original stage clock; full CLI columns include normal bootstrap and helpers.

| Rules/contacts | Messages | Cold function body | Unchanged function body | Emission body | Full unchanged sieve CLI |
| --- | --- | --- | --- | --- | --- |
| 10 | 100 | 0.105140 | 0.029683 | 0.011616 | 0.058788 |
| 10 | 1000 | 0.092140 | 0.029153 | 0.011740 | 0.053075 |
| 100 | 100 | 0.096688 | 0.036113 | 0.014124 | 0.054116 |
| 100 | 1000 | 0.101634 | 0.033868 | 0.014573 | 0.055899 |
| 1000 | 100 | 0.148601 | 0.073671 | 0.048972 | 0.087676 |
| 1000 | 1000 | 0.146041 | 0.073376 | 0.048122 | 0.087269 |

| Rules/contacts | Messages | Dry classify CLI | Apply CLI | Cold index CLI | Unchanged index CLI |
| --- | --- | --- | --- | --- | --- |
| 10 | 100 | 0.045378 | 1.036039 | 0.239976 | 0.129881 |
| 10 | 1000 | 0.167156 | 8.914435 | 0.382082 | 0.134496 |
| 100 | 100 | 0.052199 | 1.018971 | 0.238335 | 0.114997 |
| 100 | 1000 | 0.192422 | 8.906134 | 0.394259 | 0.127177 |
| 1000 | 100 | 0.091709 | 1.160336 | 0.245866 | 0.114312 |
| 1000 | 1000 | 0.253856 | 9.378945 | 0.397967 | 0.141494 |

Cold `update_filters` remains an unequal-work explanatory row: native builds
all policy products, while legacy builds only shell arrays. The earlier matched
small observation remains explicit: 72.294 ms legacy versus 118.958 ms native
(**+46.664 ms** including bootstrap), rather than being relabeled a full-CLI
regression. The supplementary equal-work **cold actual `jaro sieve`** comparison,
with no application filter-cache artifacts for either engine, measured medians
of **180.727 ms legacy / 130.905 ms native**, three interleaved runs each. All
original fixture bytes and subsequent independent 100-message application
checks passed. The final full matrix's function wall/body costs remain in the
raw phase distributions, including slower observations.

### Complete declared filter/index flows

For each engine/workload/repetition, add its application CLI wall time to that
same repetition's cold or unchanged index CLI wall time, then take the median
of those three sums. These are derived costs of two complete CLI operations,
not sums of independently chosen medians or a timed single fetch invocation.
The unchanged-index component is the original rescan of unchanged post-apply
messages. No separate comparative fetch-pipeline timer is available; installed
workflow tests establish orchestration correctness with fixture transport.

| Rules/contacts | Messages | Current legacy apply+cold index | Native apply+cold index | Current legacy apply+unchanged index | Native apply+unchanged index |
| --- | --- | --- | --- | --- | --- |
| 10 | 100 | 1.391862 | 1.276418 | 1.264398 | 1.165920 |
| 10 | 1000 | 12.027825 | 9.291056 | 11.811864 | 9.054874 |
| 100 | 100 | 1.582119 | 1.258959 | 1.457682 | 1.133967 |
| 100 | 1000 | 12.911009 | 9.264299 | 12.705619 | 9.033311 |
| 1000 | 100 | 3.000485 | 1.432467 | 2.886375 | 1.268588 |
| 1000 | 1000 | 20.159921 | 9.796268 | 19.918169 | 9.520439 |

All twelve native flow medians improve, and every native three-sample range
lies entirely below its fresh current-legacy range. This supports the absence
of a material regression in the declared small-workload flow without inventing
a numerical tolerance. Index overhead remains real and visible separately.
The earlier matched small cold-index observation was **+26.467 ms**, with
nonoverlapping legacy 202.466–235.199 ms and native 248.822–260.954 ms ranges;
it is not dismissed as noise. The final matrix's current cold-index observations
are below (milliseconds). All raw repetitions are retained for root review.

| Rules/contacts | Messages | Legacy median [min,max] ms | Native median [min,max] ms | Native minus legacy ms |
| --- | --- | --- | --- | --- |
| 10 | 100 | 276.412 [207.710, 373.391] | 239.976 [236.314, 250.892] | -36.436 |
| 10 | 1000 | 361.406 [359.664, 361.751] | 382.082 [376.620, 408.611] | +20.677 |
| 100 | 100 | 233.934 [221.330, 339.253] | 238.335 [229.472, 241.645] | +4.401 |
| 100 | 1000 | 347.082 [346.973, 354.489] | 394.259 [358.165, 402.014] | +47.177 |
| 1000 | 100 | 229.821 [210.295, 280.969] | 245.866 [234.040, 272.132] | +16.045 |
| 1000 | 1000 | 366.451 [366.420, 400.282] | 397.967 [374.643, 427.141] | +31.516 |

Native indexing retains the approved corrected destination layout: conflicts
select `from-routes` and known-only senders select `known`, while the legacy
characterization retains its earlier routing defects. Every batch remains
complete and byte-identical, and notmuch counts every message. No routing or
index work was normalized away to make the comparison faster. The measurement
reports this changed layout without attributing index overhead to a proven cause.

### Processes, threads and memory

Every native phase has identical selected-executable counts across all six
workloads and all three profile repetitions. Classification and application
invoke exactly one compiler and one batch helper, with zero `maddr`, `mseq` or
`mpick` calls. These selected counts are the same bounded proxy defined above;
they are not a complete syscall/fork trace. A separate normal-binary `/proc`
observation at each actual workload size confirms the fixed pool: 16 workers,
one signal manager and one coordinator. It runs after the timing matrices,
with no preloads; its wall times do not enter performance medians.

| Rules/contacts | Messages | Mode | Observed peak tasks | Observed peak FDs | Observed resident high-water KiB | Observed virtual high-water KiB |
| --- | --- | --- | --- | --- | --- | --- |
| 10 | 100 | normal | 18 | 152 | 3088 | 1151008 |
| 10 | 1000 | normal | 18 | 155 | 3552 | 1446532 |
| 100 | 100 | normal | 18 | 152 | 5352 | 1716840 |
| 100 | 1000 | normal | 18 | 154 | 8096 | 1627340 |
| 1000 | 100 | normal | 18 | 152 | 11540 | 1141968 |
| 1000 | 1000 | normal | 18 | 154 | 31244 | 1707720 |
| 10 | 100 | FD64 singleton fallback | 1 | 15 | 2652 | 3520 |
| 1000 | 1000 | FD64 singleton fallback | 1 | 18 | 30764 | 32048 |

These are sampled observations, one run per resource case, rather than wall
medians or guaranteed unseen maxima. The two low-FD runs apply the actual
original 100/1,000-message fixtures successfully through the generic singleton
fallback before pool effects. All eight resource cases verify 4,400 exact
messages and unchanged generation products. Indeterminate/short name limits,
aliases/noops/dependencies, cancellation, output failures and private recovery
have current focused normal/ASan/UBSan/TSan evidence as well.

The source bounds the pool to 16 workers, 32 prepared decisions and 17 MiB of
configured worker/signal stacks, with FD preflight for 64 source/parent FDs,
128 private storage FDs and a 32-FD reserve above existing descriptors. Seventeen
MiB describes those stacks alone, not total virtual mappings: the much larger
observed virtual high-water values are disclosed above. Inclusive per-phase
wait4 RSS medians remain in the full matrix with the launcher-floor caveat;
small differences cannot establish an isolated engine memory improvement.

A separate large-selection diagnostic, three interleaved runs per helper/size,
classified all 606,000 selected records. At 100,000 selections, median peak RSS
was **35,136 KiB retained singleton / 47,864 KiB current** (12,728 KiB higher).
Both were 21,108 KiB at 1,000 selections, an inherited launch-memory floor.
These are repeated valid selections from the original fixture, not 100,000
unique apply jobs. Both helpers stayed at one process/thread; 100,001 selections
were rejected before output or mail effects. The added parent identity increases
the maximum selection-vector capacity by 18 MiB on this ABI. This is a bounded
memory cost, not an overall memory improvement.

These historical measurements preceded the signal-boundary correction and native-default rollout. Their evidence is retained in
`.gestalt/evidence/native-filter-execution/l7/final-performance/` in the planning
workspace: `run-manifest.json`, `comparison.json`, both complete matrices, paired
per-repetition flow totals, source identities, resource transitions and raw logs.
Earlier failed experiments and measurement series remain unchanged. Reproduce
the final matrices with the commands at the start of this document; add
`--engine legacy --no-profile --runs 3` for the fresh timing-only controls.

## Native-default refresh (2026-11-06)

The current signal-corrected helper was rebuilt and measured again: 288 native
phase checks and 144 fresh legacy controls, all six original workloads, three
dedicated uninstrumented repetitions and separate native profiling repetitions.
Original fixture hashes, expected tables, exact IDs/destinations/body accounting,
host, runtime and optimized flags match. No OS cache flush, tests, builds or
diagnostic preloads overlap wall measurements. These results supersede earlier
current-tree claims; old binaries, failures and measurements remain historical.

For 1000 rules/contacts/messages, full actual `jaro sieve` has an 80.718 ms native
median: **13.903x** versus accepted L1 legacy and 25.983x versus fresh legacy.
Dry classification has a 249.884 ms median: **78.727x** and 83.819x respectively.
Both original 5x/3x gates pass. All 12 paired apply-plus-cold/warm-index ranges
in the full matrices are below fresh legacy ranges; small cold-flow medians are
1.334915 s native / 1.420622 s legacy. Selected subprocess dictionaries remain
constant across sizes, with one compiler and one batch for classify/apply.

Explanatory costs remain visible: the small cold function generates all native
artifacts versus legacy arrays only, with 48.786 ms additional wall time. It is
unequal work. Current individual cold-index and warm-index medians are 15.545 ms
and 24.387 ms higher than legacy; these are real overheads, not relabeled noise.

Separate balanced interleaved small controls initially contradicted the full
matrix. Exactly one additional three repetitions per selector was taken on the
same installed runtime without rebuilding or changing inputs/phases/cache rules.
Every sample is retained. Apply-plus-index median seconds are:

| Series | Default cold / warm | Explicit native cold / warm | Legacy cold / warm |
| --- | --- | --- | --- |
| Initial 3 | 1.691685 / 1.427477 | 1.501184 / 1.372197 | 1.477830 / 1.303243 |
| Additional 3 | 1.330186 / 1.211799 | 1.348524 / 1.228481 | 1.375402 / 1.280804 |
| All 6 | 1.431813 / 1.280805 | 1.391821 / 1.257762 | 1.455367 / 1.292024 |

Comparable cold full-CLI medians in milliseconds also retain both series:

| Series | Default | Explicit native | Legacy |
| --- | --- | --- | --- |
| Initial 3 | 181.811 | 136.545 | 160.637 |
| Additional 3 | 153.883 | 145.174 | 178.632 |
| All 6 | 160.205 | 140.859 | 163.533 |

All-six medians favor native, but the initial slower series and broad overlapping
ranges remain evidence. The selector paths and binaries match; the timing
variation has not been causally attributed. These observations are not a new
threshold, a discarded failure, a claim of noise, or a blanket claim that every
invocation is faster. No separately timed live fetch/provider pipeline is claimed.

Eight separate normal-binary resource cases verify 4400 messages: six workloads
have 18 tasks (16 workers, coordinator, signal manager), with sampled FD peaks
152–159. Generic FD64 fallback cases have one task and 16/18 FDs. Normal sampled
RSS high-water values range 2960–30800 KiB, while virtual high-water mappings
range 1201260–1856096 KiB; virtual reservation is not resident memory. Samples do
not prove unobserved peaks. The earlier 100000-selection RSS increase remains a
historical bounded-cost diagnostic, not a current overall-memory improvement.
Current unchanged-input parser-bypass, product identity, source snapshots,
profiling and exact resource transitions are retained under
`.gestalt/evidence/native-filter-execution/l7/l73/release/`, alongside all raw
matrices and the initial/additional/pooled control statistics. Release acceptance
and whole-branch terminal review remain separate.
