# Maildir deduplication benchmarks

Run from the checkout with Python 3.9+ on Linux. Set `work` to an existing
directory on the test disk, `cli` to an installed `bin/jaro`, `helper` to its
`share/jaromail/bin/jaro-dedup`, and `build` to the corresponding CMake build
with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`. Use fresh fixture and clone names.

```sh
mkdir -m 700 "$work/scratch" "$work/home"
export HOME="$work/home" PYTHONDONTWRITEBYTECODE=1
unset JAROWORKDIR
corpus=extras/bench/dedup_corpus.py
python3 "$corpus" generate "$work/small-100k" --seed 73 --count 100000
python3 "$corpus" generate "$work/small-1m" --seed 73 --count 1000000 --duplicate-percent 80
python3 "$corpus" generate "$work/unique" --profile attachment-unique --count 5
python3 "$corpus" generate "$work/repeated" --profile attachment-repeated --count 16 \
    --sizes-mib 1,8,64 --duplicate-percent 25
python3 "$corpus" verify "$work/small-1m"
python3 "$corpus" environment "$work/small-1m" --scratch "$work/scratch" \
    --helper "$helper" --build "$build" >"$work/environment.json"
python3 extras/bench/dedup_corpus_check.py --work "$work" --cli "$cli"
```

The small workload cycles through 29 sizes. For a requested duplicate
percentage the generator assigns that fraction of the total entries to extra
copies, rounded down, with at least one original. Half the extra copies belong
to one group, and the rest are distributed across groups. Each entry gets
its own file, ranked location, filename, flags and mode from the fixed seed.
The generator reserves space for filesystem blocks, metadata and a 512 MiB
margin. Check free space for clones, output and database scratch separately.

The attachment examples include a physically written 1025 MiB file and
multiple different contents with identical sizes and matching 16 KiB ends.
Change `--sizes-mib` and `--count` to vary their distribution. The generator
writes data through the whole file. On a compressing or deduplicating storage
stack, record those features too. `allocated_bytes` sums file `st_blocks * 512`,
which measures filesystem allocation and can include shared extents.

Each source contains `mail/box0` through `mail/box3`. The tab-separated
`groups.tsv` holds the content ID, size, SHA-256 and expected forward/reverse
survivor paths. The grouped `paths.tsv` records each occurrence's content ID,
octal mode and relative path. `verify` checks every expected entry's type,
size, SHA-256 and mode, counts actual entries, and checks manifest coverage.
Use `--survivors` after apply and `--reverse` for reversed folder priority.
The focused test also groups small fixtures by their complete bytes using the
existing independent oracle. It verifies preview pairs, keeper inodes and modes,
retry behavior and exact comparison under an intentionally colliding digest.

## Matched runs

Create a fresh clone for each mutating invocation, including the reference.
Prepare the cache on the actual run copy before timing. `warm` reads every
payload. `cold-hint` fsyncs each file and requests `POSIX_FADV_DONTNEED` for
its payload. This is advisory, leaves metadata cached, and does not change
global cache settings. Record memory and storage conditions per run.

```sh
python3 "$corpus" clone "$work/repeated" "$work/trial"
trial="$work/trial"
python3 "$corpus" verify "$trial"
python3 "$corpus" cache "$trial" warm
set -- "$trial/mail/box0" "$trial/mail/box1" "$trial/mail/box2" "$trial/mail/box3"
/usr/bin/time -v -o "$work/cli.time" "$cli" dedup --scratch-dir "$work/scratch" -- "$@"
python3 "$corpus" verify "$trial" --survivors
```

For the streaming full-hash reference, repeat clone, verification and cache
preparation with a fresh destination. Time only the following command.

```sh
/usr/bin/time -v -o "$work/reference.time" python3 extras/bench/dedup_full_hash.py \
    "$trial" --scratch-dir "$work/scratch" --apply
python3 "$corpus" verify "$trial" --survivors
```

For preview, use `"$cli" -n dedup` or omit the reference's `--apply`, capturing
stdout to a file in either case. Compare each report against its own fixture's
absolute paths with `expected-preview`, sorting both files under `LC_ALL=C`
outside the timing interval. Reverse the CLI operands and use `--reverse` for
the reference, verification and expected report to check reversed priority.
Keep fixture generation, cloning, cache preparation, sorting and verification
outside all timings. Use the same report destination type and scratch filesystem
for both implementations.

The Python reference visits stable disposable files in priority order, hashes
each once with SHA-256 and stores candidate representatives and pairs in SQLite.
A digest match requires a second, streaming byte comparison. It completes the
classification before deleting, holds advisory locks while applying and
validates file identities before unlink. Its apply mode requires a generated
clone. Report its total runtime as an implementation comparison. Python
iteration, its SQLite build and filesystem operations contribute to that time.

## Measuring the stages

Configure a separate Linux build for the instrumented helper. Use the same
compiler, build dependencies and pinned SQLite as the installed release.

```sh
cmake -S . -B "$work/profile-build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG -fno-omit-frame-pointer' \
    -DJARO_BUILD_TEST_DRIVERS=ON -DJARO_DEDUP_PROFILE=ON
cmake --build "$work/profile-build" --target jaro-dedup-test
profile="$work/profile-build/bin/jaro-dedup-test"
python3 extras/bench/dedup_measure.py "$work/repeated" --out "$work/repeated-runs" \
    --cli "$cli" --profile "$profile" --scratch "$work/scratch" --gnu-time /usr/bin/time
```

The default matrix uses three repetitions per implementation, cache state and
mode. Alternate implementation order on successive repetitions. The wall timer
covers GNU time and command startup through exit. GNU time supplies peak RSS,
faults, CPU time and filesystem I/O. Every successful run has a fresh
`result.json`, a complete GNU time capture, stderr and verification output.
Each preview's sorted report hash must match the manifest. Both modes also
check resulting names, content and mode against the manifest.

The profiling driver runs the same inventory, reduction, proof and application
functions as the installed helper. Its scan timer excludes the SQLite insert
callback. The index timer covers inserting rows and building the inventory
index. Sample and hash include opening and checking the message, reading its
contents, digesting and closing it. Compare measures the exact byte comparison
and its final validation. Unlink measures the successful unlink system call.
The remainder includes startup, other SQLite queries, opening comparison
files, revalidation, preview formatting and cleanup. Counters distinguish
sample, hash, provisional-pair and hash-proof reads. The difference between
total opens and those four counts gives the extra revalidation opens.

On Linux the driver observes all scratch file lengths before SQLite's Unix
VFS closes, truncates or unlinks a file. This captures the peak combined
logical scratch size, including unlinked temporary files, even for short
runs. The external sampler also records visible and open-unlinked filesystem
blocks and logical lengths every 25 ms. Its maxima are lower bounds.
Host `Cached`, `MemAvailable`, `Shmem`, `Dirty`, `Writeback` and memory
pressure stall totals include other processes. The runner also records the
occupancy of the mounts at `/tmp` and `/dev/shm` separately from resident
shared memory. This host mounted both as tmpfs. `cold-hint` can reduce
payload cache residency while leaving directory entries and inode metadata warm.

The optional digest trial links the same profiling objects against a
benchmark-only OpenSSL wrapper. Keep the original CMake helper for the default
arm. With a Ninja single-configuration build, this Bash recipe produces the
alternative for both native accelerated SHA-256 and BLAKE2s-256.

```sh
cc -O3 -g -fno-omit-frame-pointer -std=c99 -Isrc -c \
    extras/bench/dedup_digest_probe.c -o "$work/wrap.o"
mapfile -d '' -t objs < <(find "$work/profile-build/CMakeFiles/jaro-dedup-test.dir" -name '*.o' -print0)
cc -O3 -g -fno-omit-frame-pointer "${objs[@]}" "$work/wrap.o" \
    "$work/profile-build/libjaro-dedup-sqlite.a" \
    -Wl,--wrap=jf_sha256_init,--wrap=jf_sha256_update,--wrap=jf_sha256_hex \
    -lcrypto -o "$work/digest-probe"
```

Use the same measurement command with `--profile "$work/digest-probe" --impl
profile --cache warm --mode preview` and a fresh output directory. Set
`JARO_BENCH_BLAKE2S=1` for BLAKE2s-256. Both alternatives retain exact proof
and manifest validation.

## Large-index and failure checks

`extras/bench/dedup_index_fixture.py DIRECTORY` makes 2,100,000 sparse
messages across 16 Maildirs. Each has a 251-byte basename and a distinct
length. Use a fresh directory and reserve inodes plus disk scratch for the
index. Run the installed CLI under a process address-space limit, then
verify the complete fixture independently.

```sh
python3 extras/bench/dedup_index_fixture.py "$work/index-corpus"
prlimit --as=1073741824 -- "$cli" -n dedup --scratch-dir "$work/scratch" -- \
    "$work"/index-corpus/box*
python3 extras/bench/dedup_index_fixture.py "$work/index-corpus" --verify
```

The instrumented driver also accepts `--preview --test-hold` with the same
scratch and roots. It prints a private scratch directory after completing
the inventory index. Stat its `index.sqlite`, then send a single `x` on
stdin to release the held process through classification and preview.
These sparse metadata fixtures are excluded from attachment I/O throughput.

On the native host the 2.1M-entry main index occupied 1,239,322,624 logical
bytes. Both the held driver and installed CLI completed preview under a
1,073,741,824-byte address-space limit. The physical host had 64,865,932 KiB
RAM, so its filesystem cache could hold the index. Independently, a
networkless 512 MiB x86_64 QEMU guest inventoried 900,000 empty messages on
read-only ext4. Its packaged static helper built a 530,096,128-byte main
index on writable ext4. This exceeded all 484,282,368 bytes of guest RAM,
with swap disabled and peak helper RSS of 278,724 KiB. The guest result covers
inventory only.

The native large-index driver peaked at 270,068 KiB RSS and its combined
logical scratch high water was 1,682,835,991 bytes. The installed CLI
peaked at 270,392 KiB. Both reported zero duplicates on the 2.1M unique
lengths. Verification found zero allocated message-body blocks.

A separate address-limited check forced full reads of three physically
allocated 1025 MiB attachments with the same length and ends. Generate this
case with `--profile attachment-repeated --sizes-mib 1025 --count 3
--duplicate-percent 34`. The profiling preview hashed 3,224,371,200 bytes
and read 2,149,580,800 bytes for an exact comparison. It used 3,488 KiB peak
RSS. The installed CLI removed one duplicate from its clone at 3,844 KiB
peak RSS. Both survivors matched their manifest.

To recreate the guest inputs, generate the 2,048-Maildir layout and build
a dedicated image on local disk. Use new paths for both commands.

```sh
python3 extras/bench/dedup_index_fixture.py "$work/guest-source" --guest-inventory
truncate -s 4G "$work/guest-corpus.ext4"
mke2fs -q -t ext4 -F -N 975000 -O ^has_journal -d "$work/guest-source" "$work/guest-corpus.ext4"
```

Attach the corpus image read-only and a separate writable ext4 scratch disk
to a 512 MiB x86_64 QEMU guest with no swap. Mount the corpus at `/source`
and scratch at `/scratch`. Run the packaged static helper as
`jaro-dedup --inventory --scratch-dir /scratch -- /source/box*`. Record
guest `/proc/meminfo`, helper peak RSS and the maximum observed length of
`/scratch/jaro-dedup-*/index.sqlite` during inventory.

A separate 32 MiB ext4 scratch volume in the guest ran out of space during
SQLite commit. The helper returned exit 1 and cleaned its private scratch.
The native test suite also exercised bounded cache and SQLite heap settings,
and a page limit that produced `SQLITE_FULL`. A native scratch-write ENOSPC
injection during installed CLI apply preserved all 2,048 fixture entries.
Retry on available scratch removed the expected 1,638 duplicates.

## Qualification host

Measurements on 2026-11-23 used an AMD Ryzen 7 PRO 8700GE with 16 logical
CPUs, Linux 6.12.116 x86_64 and GCC 14.2.0. Inputs, SQLite scratch, clones
and sorted reports shared an ext4 filesystem on `/dev/md3`, a RAID1 pair of
NVMe partitions. `/tmp` and `/dev/shm` were separate tmpfs mounts. GNU time
1.9 supplied the resource measurements. Linux denied both unprivileged
`perf stat` and `perf record` with `perf_event_paranoid=3`.

The release used `-O3 -DNDEBUG` and pinned SQLite 3.54.1. The profiling
driver added debug information and frame pointers. The baseline ran on
Python 3.13.5 with SQLite 3.46.1 and OpenSSL 3.5.7. The optional digest
trial used the same host OpenSSL. The packaged x86_64 and arm64 static
release checks use the pinned Dyne musl toolchains.

| Fixed-seed corpus | Entries | Exact survivors | Logical bytes | File allocation bytes |
|---|---:|---:|---:|---:|
| Unique-size attachments | 5 | 5 | 1,152,385,025 | 1,152,389,120 |
| Repeated-size attachments | 16 | 12 | 317,718,528 | 317,718,528 |
| Small, zero duplicates | 100,000 | 100,000 | 74,196,892 | 409,600,000 |
| Small, high fan-out | 1,000,000 | 200,000 | 534,788,456 | 4,096,000,000 |

## Throughput and memory

The [per-run CSV](../extras/bench/dedup-results.csv) contains 126 verified
invocations. It gives absolute throughput, wall time, stage counters, I/O,
scratch and RSS. Empty stage fields mean that the corresponding implementation
did not report that counter. Preview report digests include the absolute
fixture paths. The baseline's apply validation opens are derived from its two
checked opens for each successful unlink.

Times and logical MiB/s below are medians with the minimum and maximum in
parentheses. Messages/s is the median. The attachment and 100k cases used
three runs per implementation, cache state and mode. The 1M workload used
three warm previews per implementation and one warm apply on each fresh clone.
The 1M profiling apply used `--profile-cache-kib 16384 --profile-heap-kib
65536`. The installed CLI and reference used their respective defaults.
The logical MiB/s numerator includes the whole input archive. Actual body
read bytes and filesystem input blocks are separate CSV columns.

### Unique-size attachments

| Cache | Mode | Installed elapsed s | Reference elapsed s | Installed messages/s | Reference messages/s | Installed logical MiB/s | Reference logical MiB/s |
|---|---|---:|---:|---:|---:|---:|---:|
| warm | preview | 0.007 (0.007–0.008) | 0.722 (0.717–0.761) | 679.80 | 6.93 | 149419.60 (137980.35–152177.47) | 1522.50 (1445.04–1533.29) |
| warm | apply | 0.007 (0.007–0.008) | 0.669 (0.664–0.705) | 683.35 | 7.47 | 150200.69 (137047.93–154592.73) | 1642.74 (1558.51–1654.34) |
| cold-hint | preview | 0.007 (0.007–0.008) | 0.949 (0.927–0.979) | 692.39 | 5.27 | 152187.84 (144783.94–157253.85) | 1158.35 (1122.60–1185.71) |
| cold-hint | apply | 0.008 (0.008–0.008) | 0.918 (0.891–0.946) | 609.85 | 5.44 | 134044.94 (129680.08–141928.91) | 1196.57 (1161.78–1233.38) |

### Repeated-size attachments

| Cache | Mode | Installed elapsed s | Reference elapsed s | Installed messages/s | Reference messages/s | Installed logical MiB/s | Reference logical MiB/s |
|---|---|---:|---:|---:|---:|---:|---:|
| warm | preview | 0.761 (0.759–0.769) | 0.286 (0.280–0.286) | 21.02 | 55.99 | 398.16 (394.09–399.34) | 1060.35 (1057.98–1083.43) |
| warm | apply | 0.765 (0.763–0.774) | 0.290 (0.282–0.295) | 20.93 | 55.26 | 396.27 (391.50–396.97) | 1046.51 (1027.76–1072.92) |
| cold-hint | preview | 0.858 (0.856–0.866) | 0.346 (0.339–0.354) | 18.64 | 46.25 | 352.98 (349.85–353.97) | 875.81 (854.76–893.63) |
| cold-hint | apply | 0.875 (0.853–0.875) | 0.341 (0.339–0.356) | 18.29 | 46.87 | 346.34 (346.14–355.34) | 887.50 (851.69–893.28) |

### 100,000 small messages

| Cache | Mode | Installed elapsed s | Reference elapsed s | Installed messages/s | Reference messages/s | Installed logical MiB/s | Reference logical MiB/s |
|---|---|---:|---:|---:|---:|---:|---:|
| warm | preview | 1.864 (1.863–1.867) | 2.067 (2.032–2.076) | 53,643.05 | 48,383.15 | 37.96 (37.89–37.99) | 34.24 (34.09–34.82) |
| warm | apply | 1.893 (1.882–1.897) | 2.083 (2.066–2.111) | 52,838.36 | 48,008.36 | 37.39 (37.30–37.60) | 33.97 (33.52–34.24) |
| cold-hint | preview | 9.617 (9.530–9.709) | 10.207 (10.151–10.211) | 10,398.28 | 9,797.21 | 7.36 (7.29–7.42) | 6.93 (6.93–6.97) |
| cold-hint | apply | 9.714 (9.599–9.766) | 10.543 (10.429–10.552) | 10,294.46 | 9,484.73 | 7.28 (7.25–7.37) | 6.71 (6.71–6.79) |

### 1,000,000 small messages

| Cache | Mode | Installed elapsed s | Reference elapsed s | Installed messages/s | Reference messages/s | Installed logical MiB/s | Reference logical MiB/s |
|---|---|---:|---:|---:|---:|---:|---:|
| warm | preview | 63.158 (63.130–63.188) | 49.225 (49.164–49.836) | 15,833.20 | 20,314.71 | 8.07 (8.07–8.08) | 10.36 (10.23–10.37) |
| warm | apply | 78.932 (one run) | 81.493 (one run) | 12,669.08 | 12,271.06 | 6.46 (one run) | 6.26 (one run) |

### Stage costs and tuning decision

The profiling driver recorded the following warm apply times. Times are
medians for the three-run cells. Scratch and RSS columns give maxima. The
1M row is the single reduced-memory apply. All four workloads used file-backed
SQLite on the task disk.

| Corpus, warm apply | Scan ms | Index ms | Sample ms | Hash ms | Compare ms | Unlink ms | Opens | Body read MiB | Peak scratch MiB | Peak RSS MiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Unique-size attachments | 0.108 | 1.001 | 0.000 | 0.000 | 0.000 | 0.000 | 0 | 0.00 | 0.028 | 3.41 |
| Repeated-size attachments | 0.127 | 1.014 | 1.480 | 746.015 | 1.996 | 0.023 | 48 | 325.50 | 0.028 | 3.45 |
| 100,000 small messages | 139.193 | 108.618 | 0.000 | 1,500.881 | 0.000 | 0.000 | 100,000 | 70.76 | 20.773 | 24.05 |
| 1,000,000 small messages | 1,381.972 | 1,313.718 | 0.000 | 14,591.527 | 6,416.206 | 6,873.907 | 4,200,000 | 1,247.00 | 278.600 | 38.18 |

The unique-size workload incurred zero message opens and zero body reads.
Its large logical MiB/s figures reflect the archive size a metadata-only
inventory could process. After cold hints, the median GNU time filesystem
input count was zero for the installed preview and 2,250,760 blocks of
512 bytes for the reference. For repeated-size attachments both reported
620,544 input blocks under cold hints and zero under warm preparation.

In the 1M warm preview each of the three profiling runs made 1,000,000 hash
opens, 1,600,000 exact-proof opens and 1,600,000 final validation opens.
Each proposed the same 800,000 removals. The installed warm apply removed
386,392,304 logical bytes and 3,276,800,000 last-link allocated bytes. The
reduced-cache driver retained all 200,000 manifest winners, used 39,092 KiB
peak RSS and took 80.121 s.

Hashing dominated the repeated-size attachment profile. The following
benchmark-only replacements changed the digest implementation in the same
optimized driver. Each of the three warm previews per arm hashed 317,718,528
bytes, carried out the same byte proofs and matched the same preview report.

| Repeated attachments, warm preview | Elapsed s | Hash stage ms |
|---|---:|---:|
| First-party SHA-256 | 0.758 (0.756–0.823) | 749.8 (746.9–814.4) |
| OpenSSL SHA-256 | 0.163 (0.162–0.164) | 154.5 (153.4–154.7) |
| OpenSSL BLAKE2s-256 | 0.445 (0.440–0.449) | 433.6 (430.1–439.7) |

The fastest measured digest arm used OpenSSL SHA-256 on this x86_64 host.
The shipping helpers keep the first-party SHA-256 implementation and the
single SQLite backend. Adding OpenSSL would introduce a target crypto
library and require static-package and arm64 performance qualification.
On the 2.1M sparse workload the measured scan took 4.52 s and inventory
index work took 4.43 s. The storage results cover serial reads with the
current index.

Across 126 timed invocations, the installed CLI peaked at 217,804 KiB RSS.
Host cached pages ranged from 34.86 to 38.65 GiB, and minimum host available
memory was 46.01 GiB. Resident shared memory at run boundaries ranged from
8.60 to 10.11 GiB. The `/tmp` tmpfs had 23.73 GiB occupied, and `/dev/shm`
had zero allocated blocks at the recorded endpoints. The ext4 scratch
filesystem had 29,577,776 KiB available at 15:07 UTC. The largest host-wide
full-memory-stall delta in any timed invocation was 8,473 microseconds.
The sampler obtained 39,841 observations and recorded six inaccessible
procfs samples. The driver’s in-process scratch high water remains available
independently in the profiling rows.
