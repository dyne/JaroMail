# Reproducing the legacy filter baseline

Run from a dependency-equipped source checkout:

```sh
zsh extras/bench/filter-baseline.sh --output /absolute/empty/report-directory --runs 3
```

The entry point snapshots source before building/staging, so generated manuals
do not modify the checkout. It uses existing test helpers and a private install;
fixture creation, staging and verification are outside measured operations.
`--sizes 10 100 1000 --messages 100 1000` are the defaults. Each workload gets
three independent timing repetitions and three independent instrumentation
repetitions. The strict suite runs a smaller harness regression with 10 rules,
10 contacts, eight messages and one repetition; that smoke run is not a timing
baseline. Output must be empty to prevent evidence replacement.

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
