# Shared native policy core

The policy consumes immutable snapshots; the classifier returns a borrowed
winning rule and performs no IO. `headers.h` supplies an owned message view from
either a byte buffer or a caller-owned descriptor. Zsh remains the application
default; no Dovecot library is linked here.

The header reader scans once to the empty LF/CRLF separator. Its 1 MiB wire limit
includes line endings and the separator. A physical line payload (excluding its
ending) and each unfolded field value are independently limited to 64 KiB.
Unfolding removes only the line ending, retaining the continuation's SP/HTAB.
Unknown fields still receive syntax, physical-line and unfolded-size checks.
Body bytes, including NUL or arbitrary binary content, are never interpreted.
Descriptor reads use 4096-byte chunks and can consume up to 4095 body bytes
beyond the separator; callers retain descriptor ownership and must not share
its offset. No seek, write, deletion, or message-content modification occurs.

Repeated relevant fields remain separate. Missing relevant fields and empty
groups are valid nonmatches. Empty address field values, malformed lists,
truncation and unsupported address forms are explicit errors, with no usable
partial message. Unfolded Sender values are retained separately for bounce
matching; spam compares after ignoring outer SP/HTAB. Address predicates only
receive extracted mailbox strings, preserving quoted local parts and escaped
bytes. Display names (including encoded words), comments and group names do not
become mailbox values. No display-name or body MIME decoding is needed.

The bounded address-list entry point lives in the existing `rfc822.c` module
and reuses its quote/comment lexer. The legacy `rfc822_parse_adrlist` cannot be
called safely for this contract: its STRING=256 token buffers silently truncate
long mailboxes and its helpers terminate on allocation failure. Audit probes
also demonstrated a dropped escaped quote byte and a null-tail semicolon crash.
Those two defects, a failed-addrspec leak and end-of-string token advancement
were repaired narrowly for legacy callers; fixed-buffer legacy behavior was not
expanded into a new general parser. The native entry point uses caller scratch,
no fatal allocator and no global error state. Its tentative callback output is
published only when the entire header block succeeds.

Supported address syntax covers dot-atom or quoted local parts, ASCII/A-label
domains, CFWS, name/angle addresses, comma-separated lists and nonnested groups,
including empty groups. Source routes, domain literals, non-ASCII domains,
obsolete mixed quoted/dot local forms and malformed separators fail explicitly.
UTF8 local parts are preserved with ASCII-only case folding by policy matching.
No new parser dependency or general MIME/Sieve implementation is introduced.
