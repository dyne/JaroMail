# Installed contact latency

The legacy baseline is `a6810a0d7e6d9604aa0491d06d3840cccd42fe7d`, the last
revision before JaroMail switched these callers to SQLite. The SQLite stage
uses `56ea41cb755e6067246597ecd16b2d8294240e81` plus the parser and editor
qualification patch with SHA-256
`0cbd6288f5e4406af54fd688ea222f30b7694e8fde2ff1723caaca6664fe84a3`.
Both report JaroMail 6.4.0. Their private abook builds identify themselves as
`0.6.2+jaromail.3` and `0.6.2+jaromail.4`, respectively.

## Reproduce

From the candidate checkout, set `SIEVE_DEPS_PREFIX` to a prepared native
Sieve dependency prefix that both revisions can use. The example builds Release
stages with the same toolchain and stores the fixtures, binaries, and results
outside the checkout.

```sh
repo=$PWD
work=$(mktemp -d /path/to/writable-scratch/contact-latency.XXXXXXXX)
git worktree add --detach "$work/legacy-source" a6810a0d7e6d9604aa0491d06d3840cccd42fe7d
export CCACHE_DIR="$work/ccache"
export CCACHE_TEMPDIR="$work/ccache-tmp"
for variant in legacy sqlite; do
    source=$repo
    if [ "$variant" = legacy ]; then source=$work/legacy-source; fi
    cmake -S "$source" -B "$work/$variant-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache "-DJARO_SIEVE_DEPS_PREFIX=$SIEVE_DEPS_PREFIX"
    cmake --build "$work/$variant-build" --parallel
    cmake --install "$work/$variant-build" --prefix "$work/$variant-prefix"
done
python3 extras/bench/contact_latency.py \
    --legacy "$work/legacy-prefix" --sqlite "$work/sqlite-prefix" \
    --source "$repo" --out "$work/results" --time-command /usr/bin/busybox time
```

The harness defaults to GNU `time` if `--time-command` is omitted. It writes
installed binary hashes, input digests and query plans, preflight results, one
JSON record per trial, and a summary. Every case checks expected status and
ordered rows on both installations before timing begins. The direct query
helpers use their own documented miss framing. The legacy user's abook config
requests one completion candidate per contact, matching the SQLite Tab path.
Full filter export includes secondary addresses on both versions. The largest
fixture keeps 100,000 contact records and repeats eight primary memberships to
stay below the effective filter rule cap after deduplication.

End-to-end cases use installed `jaro -q complete`, `jaro -q isknown`, and
`jaro -n -q sieve`. Direct cases use the installed legacy `abook` or SQLite
`jaro-filter-compile` for completion and the installed compiler on both sides
for exact membership and filter export. The timed export does a full read-only
filter build with `--check`. Preflight checks its generation ID against a
separate publication that must contain the same ordered sender memberships
and destinations on both stages. SQLite migration, publication, and fixture
copying are outside timed trials.

Each starting measurement launches a fresh process in its own copy of the
profile. Each warm measurement launches a fresh process against the existing
profile after one unmeasured execution of the same case. Starting measures
fresh process and profile state with an already initialized SQLite database.
File pages and binaries may be cached in both conditions. No cache eviction
was attempted. The versions alternate order on each trial. Nearest-rank p95
is the highest observation with five starting and eleven warm trials. Wall
time includes the external timer. RSS is the largest process peak reported
by the timer over the command and waited-for descendants.

## Measured tradeoffs

The 2026-11-24 run used Debian 13 on x86-64 with GCC 14.2 in Release mode.
It verified 108 command outcomes and six filter publications before recording
1,728 trials. The timer control ran `/bin/true` eleven times. Its median was
1.1 ms, its maximum was 22.8 ms, and its peak RSS was 1,792 KiB. Differences
near the control median and isolated p95 spikes deserve the context of the
full distributions.

| Contacts | Legacy text bytes | SQLite bytes | Exported mailbox memberships |
|---:|---:|---:|---:|
| 100 | 7,709 | 69,632 | 93 |
| 10,000 | 779,007 | 2,433,024 | 9,993 |
| 100,000 | 7,889,006 | 24,391,680 | 99,993 |

At 100,000 contacts, warm mid-name completion fell from 122.7 to 56.2 ms
through jaro and from 99.1 to 31.5 ms directly. The SQLite helper still
scans contacts for prefix, middle, and miss cases. Its stored search keys
avoid rebuilding legacy keys and loading the text book for each invocation.
In contrast, exact membership uses the covering `addresses_book_mailbox`
index and stays near 2 ms directly at all three sizes. The observed plan for
the substring statement includes a contact scan, indexed address subqueries,
and a temporary sort to restore book and record order. The export plan
streams ordered distinct keys from the same covering index. These are plans
for the production SQL, explained with the host Python SQLite 3.46.1. The
installed candidate embeds SQLite 3.54.1.

At 100 contacts, warm exact membership increased by 0.6 ms directly and
0.7 ms through jaro. This size provides little opportunity for an index to
offset helper and database-open overhead. Warm full filter builds increased
by 4.6 ms directly and 4.8 ms through jaro, so shell dispatch accounts for
little of that difference. The direct measurement includes storage, policy
assembly, and Sieve runtime identity checks. At 100,000 contacts, warm full
filter export improved from 631.8 to 528.7 ms directly and from 657.7 to
552.9 ms through jaro. SQLite took roughly three times the storage at the
two larger sizes. At 100,000 contacts it lowered peak RSS across the measured
cases. At 10,000, direct full filter export used 18,940 KiB with SQLite and
15,564 KiB with the legacy book in warm trials. The measured completion
improvement across large books did not warrant adding another substring index
for this release.

Each cell below reports median milliseconds / p95 milliseconds / maximum KiB
of RSS.

### Installed jaro
| Contacts | Case | Starting legacy | Starting SQLite | Warm legacy | Warm SQLite |
|---:|---|---:|---:|---:|---:|
| 100 | name-hit | 30.1 / 30.4 / 4,688 | 29.4 / 30.8 / 4,688 | 26.2 / 26.7 / 4,652 | 25.9 / 27.4 / 4,684 |
| 100 | prefix | 29.7 / 29.9 / 4,688 | 31.1 / 31.2 / 4,584 | 26.1 / 26.3 / 4,688 | 25.8 / 26.4 / 4,688 |
| 100 | mid-name | 29.3 / 29.6 / 4,680 | 30.8 / 31.2 / 4,584 | 26.1 / 26.9 / 4,684 | 25.9 / 26.5 / 4,660 |
| 100 | mid-note | 29.9 / 30.3 / 4,540 | 30.1 / 30.9 / 4,688 | 26.2 / 26.7 / 4,688 | 25.8 / 26.3 / 4,688 |
| 100 | secondary | 28.9 / 29.8 / 4,672 | 30.1 / 30.9 / 4,640 | 26.0 / 26.3 / 4,688 | 26.2 / 27.4 / 4,692 |
| 100 | miss | 29.6 / 30.2 / 4,616 | 31.0 / 31.3 / 4,656 | 26.1 / 27.3 / 4,696 | 26.2 / 26.7 / 4,688 |
| 100 | member-hit | 26.4 / 28.3 / 4,684 | 28.1 / 28.7 / 4,692 | 23.2 / 24.7 / 4,684 | 23.9 / 25.2 / 4,680 |
| 100 | member-miss | 27.8 / 28.1 / 4,640 | 27.4 / 29.5 / 4,664 | 23.7 / 24.7 / 4,680 | 23.9 / 25.3 / 4,688 |
| 100 | filter-export | 52.7 / 53.6 / 4,792 | 57.5 / 60.3 / 5,364 | 52.9 / 54.9 / 4,816 | 57.7 / 59.9 / 5,476 |
| 10,000 | name-hit | 39.5 / 40.7 / 4,988 | 34.1 / 36.3 / 4,992 | 35.8 / 38.0 / 5,092 | 29.7 / 30.7 / 5,124 |
| 10,000 | prefix | 39.6 / 40.9 / 5,136 | 33.5 / 35.5 / 5,088 | 36.6 / 38.0 / 5,140 | 29.9 / 31.6 / 5,056 |
| 10,000 | mid-name | 40.5 / 41.6 / 5,012 | 33.7 / 34.1 / 5,028 | 36.2 / 37.5 / 5,056 | 29.7 / 30.9 / 4,988 |
| 10,000 | mid-note | 40.0 / 41.6 / 5,060 | 34.3 / 35.3 / 5,072 | 35.7 / 36.7 / 5,136 | 29.7 / 31.5 / 5,048 |
| 10,000 | secondary | 39.1 / 40.1 / 5,092 | 34.1 / 34.8 / 4,972 | 35.7 / 36.8 / 5,136 | 29.6 / 30.9 / 5,100 |
| 10,000 | miss | 38.3 / 39.0 / 5,136 | 33.5 / 35.4 / 4,880 | 35.7 / 36.9 / 5,092 | 29.7 / 30.5 / 5,016 |
| 10,000 | member-hit | 29.7 / 29.8 / 4,680 | 28.0 / 29.3 / 4,624 | 26.1 / 27.1 / 4,688 | 24.4 / 25.8 / 4,696 |
| 10,000 | member-miss | 29.3 / 30.3 / 4,616 | 28.6 / 29.1 / 4,608 | 25.8 / 26.4 / 4,688 | 24.3 / 25.1 / 4,688 |
| 10,000 | filter-export | 109.5 / 110.3 / 15,892 | 102.7 / 103.0 / 19,208 | 108.8 / 110.1 / 15,692 | 101.7 / 103.7 / 18,980 |
| 100,000 | name-hit | 124.3 / 126.4 / 28,252 | 59.4 / 60.4 / 5,260 | 119.6 / 122.6 / 28,244 | 55.3 / 56.3 / 5,304 |
| 100,000 | prefix | 125.8 / 126.1 / 28,232 | 60.6 / 61.6 / 5,232 | 121.9 / 130.4 / 28,256 | 56.0 / 57.1 / 5,296 |
| 100,000 | mid-name | 126.6 / 128.2 / 28,296 | 60.0 / 61.9 / 5,316 | 122.7 / 125.4 / 28,296 | 56.2 / 57.6 / 5,344 |
| 100,000 | mid-note | 124.6 / 127.8 / 28,240 | 60.8 / 61.3 / 5,320 | 121.5 / 123.2 / 28,192 | 56.3 / 57.5 / 5,320 |
| 100,000 | secondary | 122.9 / 123.7 / 28,252 | 58.8 / 59.8 / 5,312 | 118.8 / 120.1 / 28,252 | 54.8 / 56.5 / 5,324 |
| 100,000 | miss | 123.2 / 123.7 / 28,136 | 59.6 / 61.0 / 5,252 | 118.6 / 121.8 / 28,168 | 55.8 / 57.0 / 5,220 |
| 100,000 | member-hit | 44.6 / 45.3 / 9,224 | 28.5 / 30.3 / 4,688 | 40.7 / 42.3 / 9,388 | 24.4 / 25.1 / 4,532 |
| 100,000 | member-miss | 45.3 / 46.5 / 9,332 | 28.8 / 29.8 / 4,600 | 41.3 / 42.3 / 9,332 | 24.5 / 25.5 / 4,696 |
| 100,000 | filter-export | 659.7 / 668.7 / 135,072 | 555.0 / 557.3 / 128,812 | 657.7 / 663.3 / 134,108 | 552.9 / 554.5 / 127,800 |

### Direct helpers
| Contacts | Case | Starting legacy | Starting SQLite | Warm legacy | Warm SQLite |
|---:|---|---:|---:|---:|---:|
| 100 | name-hit | 2.1 / 2.1 / 2,580 | 2.0 / 2.3 / 3,264 | 2.0 / 2.0 / 2,536 | 1.9 / 2.3 / 3,292 |
| 100 | prefix | 2.0 / 2.1 / 2,532 | 2.1 / 2.3 / 3,292 | 1.9 / 2.0 / 2,580 | 2.1 / 2.2 / 3,220 |
| 100 | mid-name | 2.0 / 2.1 / 2,388 | 2.2 / 2.4 / 3,296 | 2.0 / 2.1 / 2,576 | 2.1 / 2.2 / 3,288 |
| 100 | mid-note | 2.0 / 2.1 / 2,460 | 2.2 / 2.3 / 3,272 | 2.0 / 2.1 / 2,580 | 2.1 / 2.4 / 3,264 |
| 100 | secondary | 2.0 / 2.1 / 2,532 | 2.1 / 2.1 / 3,292 | 2.0 / 2.1 / 2,576 | 1.9 / 2.2 / 3,304 |
| 100 | miss | 2.0 / 2.4 / 2,532 | 2.1 / 2.2 / 3,296 | 2.0 / 2.0 / 2,580 | 2.0 / 2.2 / 3,292 |
| 100 | member-hit | 1.5 / 1.6 / 1,784 | 2.2 / 10.7 / 3,220 | 1.4 / 1.6 / 1,788 | 2.0 / 2.3 / 3,300 |
| 100 | member-miss | 1.4 / 1.5 / 1,748 | 2.0 / 2.1 / 3,304 | 1.3 / 1.4 / 1,772 | 1.9 / 2.1 / 3,304 |
| 100 | filter-export | 28.6 / 29.9 / 3,916 | 34.0 / 34.9 / 5,404 | 28.8 / 29.4 / 4,052 | 33.4 / 34.4 / 5,428 |
| 10,000 | name-hit | 11.9 / 12.0 / 5,064 | 6.1 / 6.7 / 4,948 | 12.0 / 12.4 / 5,092 | 5.9 / 6.6 / 5,072 |
| 10,000 | prefix | 12.1 / 12.5 / 4,952 | 5.8 / 6.7 / 5,012 | 12.2 / 12.6 / 5,076 | 5.9 / 6.7 / 5,088 |
| 10,000 | mid-name | 12.1 / 12.4 / 5,092 | 6.2 / 6.7 / 5,124 | 12.2 / 13.0 / 5,140 | 5.8 / 6.3 / 5,012 |
| 10,000 | mid-note | 11.9 / 12.1 / 5,020 | 6.1 / 6.6 / 4,972 | 12.2 / 12.7 / 4,980 | 5.8 / 6.2 / 5,080 |
| 10,000 | secondary | 11.9 / 12.6 / 4,928 | 5.8 / 6.4 / 4,988 | 11.9 / 12.3 / 5,092 | 5.7 / 6.1 / 5,056 |
| 10,000 | miss | 11.8 / 13.9 / 5,020 | 5.8 / 5.9 / 4,964 | 11.7 / 12.0 / 5,076 | 5.7 / 6.3 / 5,012 |
| 10,000 | member-hit | 3.3 / 3.7 / 2,340 | 2.0 / 2.1 / 3,340 | 3.3 / 4.0 / 2,336 | 2.1 / 10.0 / 3,312 |
| 10,000 | member-miss | 3.5 / 4.0 / 2,304 | 2.2 / 2.3 / 3,224 | 3.2 / 4.1 / 2,304 | 2.1 / 2.1 / 3,304 |
| 10,000 | filter-export | 84.7 / 87.9 / 15,804 | 79.3 / 84.2 / 19,232 | 85.0 / 88.1 / 15,564 | 78.5 / 78.9 / 18,940 |
| 100,000 | name-hit | 95.2 / 96.3 / 28,196 | 30.9 / 31.4 / 5,308 | 95.7 / 97.4 / 28,360 | 31.1 / 32.2 / 5,332 |
| 100,000 | prefix | 98.2 / 98.7 / 28,188 | 31.5 / 31.7 / 5,348 | 97.2 / 98.9 / 28,360 | 31.5 / 37.5 / 5,320 |
| 100,000 | mid-name | 99.2 / 99.6 / 28,232 | 31.6 / 31.8 / 5,304 | 99.1 / 101.4 / 28,200 | 31.5 / 33.1 / 5,344 |
| 100,000 | mid-note | 97.5 / 98.7 / 28,252 | 32.3 / 33.8 / 5,264 | 97.9 / 98.9 / 28,248 | 31.7 / 32.9 / 5,316 |
| 100,000 | secondary | 93.4 / 94.7 / 28,360 | 30.0 / 30.8 / 5,228 | 94.3 / 99.2 / 28,296 | 30.6 / 30.9 / 5,284 |
| 100,000 | miss | 93.1 / 94.8 / 28,116 | 31.2 / 32.3 / 5,140 | 94.0 / 96.1 / 28,236 | 31.0 / 31.2 / 5,212 |
| 100,000 | member-hit | 18.3 / 19.5 / 9,328 | 2.5 / 2.7 / 3,288 | 18.8 / 19.2 / 9,380 | 2.1 / 2.3 / 3,252 |
| 100,000 | member-miss | 18.2 / 18.4 / 9,348 | 2.2 / 2.3 / 3,296 | 18.3 / 18.9 / 9,380 | 2.1 / 2.3 / 3,276 |
| 100,000 | filter-export | 637.2 / 639.4 / 134,952 | 532.8 / 533.8 / 128,864 | 631.8 / 662.0 / 134,176 | 528.7 / 537.5 / 127,844 |
