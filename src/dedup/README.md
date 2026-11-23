# Private Maildir inventory

This feature is under development. It has no public command or deletion path.
The native fixture driver is built only with `JARO_BUILD_TEST_DRIVERS=ON`.

Input paths containing `/`, plus `.` and `..`, resolve relative to the caller
(or as absolute paths). Bare names resolve below the configured mail root when
one is provided; they otherwise resolve relative to the caller. There is no
fallback from a missing configured folder to a same-named local directory.
The fixture driver takes `--mail-root PATH` over `JAROMAILDIR`, and accepts `--`
before names beginning with `-`. For example `./Inbox` explicitly selects a
local folder even with `JAROMAILDIR` set.

Preflight resolves all input roots before enumeration. Directory aliases are
allowed: an identical ordered pair of physical `cur`/`new` directories is
inventoried once at the first input's original zero-based rank. Sharing only
one directory, reversing the pair, or aliasing `cur` to `new` is ambiguous and
fails. Both directories must exist; `tmp` is never inspected. Each eligible
direct regular entry yields a row, including hidden names, empty files and
multiple hard links. Message symlinks, FIFOs and nested directories are ignored.
All operand root identities, including aliases that yield no additional rows,
remain in the overlap check; rejection does not depend on input order.

The scanner holds only the input-root table and the current directory stream;
it never collects an entire directory and never opens message payloads.
Directory descriptors are bounded independently of input count. Reopened
directories must match preflight device/inode identities. Enumeration/stat,
callback or binding failures make the whole scan incomplete, even if callbacks
already received rows. Consumers must publish success only after the scanner
and their own storage operations succeed. A concurrent fresh-name delivery may
be inventoried on this run or the next; existing entries should remain stable.

The test-only driver's rows contain ten NUL-terminated fields: input rank,
cur/new rank (0/1), raw basename, size, device, inode, mtime seconds, mtime
nanoseconds, ctime seconds and ctime nanoseconds. The basename is the only
nondecimal field. It is an opaque byte string; neither locale nor Unicode
conversion is involved. Order within a directory is deliberately unspecified.

## Private scratch inventory

The uninstalled `jaro-dedup --inventory [--scratch-dir PATH] [--mail-root PATH]
-- ROOT...` entry point builds the inventory and reports counters. It cannot
delete mail. It defaults to `TMPDIR`, then `/tmp`; set `--scratch-dir` to an
existing directory on local disk for large archives. A tmpfs scratch path also
uses machine memory, independently of process RSS. No message bodies enter
scratch storage.

Each invocation creates a random `jaro-dedup-XXXXXX` directory with mode 0700,
containing a named mode-0600 `index.sqlite`. SQLite's journal and temporary
files use that directory too. The single-process helper exclusively controls
`sqlite3_temp_directory` while the connection is open. Linux may expose temp
files as anonymous `O_TMPFILE` descriptors instead of `etilqs_` names. Normal
success and errors close the database and remove owned scratch files. Crashes
may leave scratch; it is disposable and never needed to find original mail.

Prepared inserts commit every 1,024 rows. `occurrences` stores integer row ID,
original input rank, cur/new rank, basename BLOB, size, decimal-text unsigned
device/inode, and seconds/nanoseconds for modification and change times.
`roots.path` is also a BLOB. A completion flag remains false until enumeration,
all commits and index creation succeed. Cursor consumers require that flag.
The `occurrence_size(size,rank,subdir,name)` index streams rows in size and
priority order without collecting a size bucket in memory; BLOB ordering is
unsigned byte order. Input-root metadata remains in the bounded root table.

The default main page cache is 128 MiB; the temporary database cache is at
most 8 MiB. Both are checked after configuration. `temp_store=FILE` and
`mmap_size=0` are checked too, and mapping is disabled at compile time.
The process sets and checks SQLite's
[hard heap limit](https://sqlite.org/c3ref/hard_heap_limit64.html) at 384 MiB,
with memory accounting enabled and no alternative allocator/page cache.
That bound includes SQLite sort and connection allocations, leaving headroom
under the initial 1 GiB process target; full-scale RSS qualification is L4.
Root-table memory scales with operand count, never message count or body size.

SQLite is [pinned and compiled from source](../../vendor/sqlite/README.md)
for both native and target-static builds. No host SQLite library is searched.
Fault controls, dump output, reduced-cache settings and the bounded inspection
handshake exist only in `jaro-dedup-test`, built with
`JARO_BUILD_TEST_DRIVERS=ON`. They are rejected by the production helper.
