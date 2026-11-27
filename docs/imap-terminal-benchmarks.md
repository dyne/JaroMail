# Installed IMAP shell release checks

Run the release check from a built checkout with the usual test dependencies,
Python 3.9+, gzip, tar and Dovecot 2.4.x with its matching doveconf. Run as
the mail owner. The script stages an installation and starts each client
through its `bin/jaro`. Both servers listen on private loopback ports.

```sh
report=$(mktemp -d)
JARO_TEST_LOG_DIR="$report" zsh extras/test/run-tests.sh extras/test/test-imap-release.sh
```

The report directory contains the runner log and a fresh `imap-release.*`
directory with per-command counts, timings, byte counters, transcripts and
verified archives. To repeat one oracle, invoke
`zsh extras/test/test-imap-release.sh --scale` or use `--dovecot`. Set
`JARO_TEST_LOG_DIR` to an existing absolute directory to retain the evidence.
`SIEVE_DEPS_PREFIX` may point to a private Dovecot installation.
Dovecot uses a short temporary directory for Unix sockets. Its mail, indexes,
configuration and logs belong to the disposable fixture.

## Dataset and measurements

The generated server has 10,104 folders and 1,999,904 logical message
records. Under `/Archive`, 100 groups each contain 100 selectable folders.
One folder holds one million messages and the other 9,999 hold 100 each.
Under `/Exports`, two folders each contain a 4 KiB message and either a
1 MiB or 128 MiB message. UID 17 starts each mailbox, with gaps of 4,093
between successive UID values. The server generates names, metadata and
message slices on request. Each export contains a synthetic header followed
by a repeating byte sequence.

The table records one run on 27 November 2026 at 10:34 UTC, Linux
6.12.116+deb13-amd64 on x86-64, glibc 2.41 and Python 3.13.5.
Command counts include connection setup and normal logout. Wire bytes cover
the IMAP stream in both directions, excluding TCP framing. Body bytes count
only message body literals. RSS is the maximum resident process size reported
by `wait4` for each launcher and its waited descendants. Staging is the highest
sum of named staging file lengths sampled about every 3 ms. Filesystem
allocation, unnamed temporary files and changes between samples are outside
that measurement.

| Installed operation | Seconds | Commands | RSS (KiB) | Client → server (B) | Server → client (B) | Body (B) | Staging (B) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Browse three levels | 0.089 | 7 | 24,548 | 166 | 10,436 | 0 | 0 |
| Read one 25-header page | 0.094 | 14 | 24,628 | 883 | 17,200 | 0 | 0 |
| Rank 10,000 folders with LIST-STATUS | 0.381 | 106 | 36,560 | 5,759 | 937,127 | 0 | 0 |
| Rank 100 folders from size metadata | 0.278 | 2,107 | 24,420 | 135,877 | 716,499 | 0 | 0 |
| Export the 1 MiB sample | 0.290 | 15 | 27,940 | 673 | 1,054,081 | 1,052,672 | 1,078,304 |
| Export the 128 MiB sample | 1.134 | 523 | 28,024 | 46,403 | 134,298,435 | 134,221,824 | 134,760,406 |
| Interrupt the large export | 0.269 | 43 | 28,128 | 3,210 | 8,399,327 | 8,393,728 | 8,409,618 |
| Resume the large export | 1.663 | 520 | 28,196 | 46,264 | 134,294,065 | 134,217,728 | 134,760,408 |

Opening the tree issued three one-level LIST requests. In the million-message
folder, the page requested 25 UIDs and 25 bounded headers in three batches
each. The fast ranking used 101 combined LIST-STATUS requests after locating
the subtree. The fallback scanned 10,000 messages across 100 folders, using
1,000 batches for inventory and 1,000 for sizes. Both rankings transferred
zero message bodies. Fallback work grows with the selected inventory.
Network round trips to a remote provider can dominate its runtime.

The large export increased observed RSS by 84 KiB over the small one. The
regression allows 24 MiB of variation and requires the large run to stay
below 96 MiB. Both workloads fetched at most 256 KiB per body request. The
repeating content compresses well. Mail containing compressed attachments can
require substantially more staging space.

The interrupted client received a partial literal after it had staged 8 MiB
of the second message. It exited 130 with the 4 KiB predecessor complete,
a private checkpoint and an absent destination. Resume requested only the
second UID and restarted it at byte zero. For both large completions the
published file had the same inode as the sampled staged archive and its full
size at first observation. Standard gzip and tar checks, extraction and
independent SHA-256 comparisons passed for every member. The uninterrupted
and resumed compressed archives were byte-identical.

## Dovecot interoperability

On rootless Dovecot 2.4.5, the installed shell created and renamed a mailbox,
read a header page and size, and downloaded its two messages. A separate
IMAP client checked the original bytes and UIDs. Standard tar extraction
recovered both original messages before the shell explicitly deleted the
renamed mailbox. An unrelated mailbox and its message survived.

This run used a CRLF text message and a MIME-encoded binary attachment.
During qualification, a raw NUL submitted via ordinary APPEND came back as
byte 0x80 on this server even to the independent IMAP client. Export fidelity
is measured against the bytes exposed by the server. These runs cover one
Dovecot configuration and an isolated generated account. A download captures
the selected folders and UID bounds independently as documented in the
[operator guide](optional-integrations.md#download-and-verify-a-folder).
