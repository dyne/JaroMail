# SQLite 3.54.1

`upstream/sqlite3.c` and `upstream/sqlite3.h` are unmodified members of
SQLite's [official amalgamation](https://sqlite.org/2026/sqlite-amalgamation-3540100.zip).
The archive's SHA3-256 was checked against the [upstream download page](https://sqlite.org/download.html)
on 2026-11-23 before extracting these two files. `sources.json` pins that
checksum, the archive's SHA-256 and the selected members' sizes and SHA-256.
Both files embed SQLite's public-domain notice. See [SQLite copyright](https://sqlite.org/copyright.html).
No shell, extensions, archive or generated build output is vendored.

CMake compiles this source into `jaro-dedup-sqlite` with the selected target
compiler, then links that archive into the private `jaro-dedup` helper. Native
and cross/static builds use these same inputs. There is no system SQLite
discovery or host library fallback. The amalgamation is configured without
threads, extension loading, WAL or double-quoted string literals. Memory
accounting remains enabled; temporary tables use files and memory mapping is
disabled. This is a private, single-process scratch database, never an input
database supplied by mail.

Run `python3 vendor/verify.py` to check the retained files. Dependency updates
must select an explicit official release, verify its published archive digest,
review the changes, update the manifest and rerun native and target builds.
