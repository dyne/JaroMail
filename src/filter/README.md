# Filter compiler

This directory builds a private policy snapshot from Filters.txt, account and
alias addresses, abook exports and the selected profile. The parser resolves
duplicates and precedence before `emit.c` emits one portable Sieve source.
`generation.c` runs the pinned compiler and verifies the resulting bytecode
before publishing it alongside its source, dependency inventory, action
provenance and manifest. Readers pin and verify a generation under the
publication lock. The importer reuses the parser, bounded RFC822 address
reader and journal to update authored inputs and their generation together.

Local execution and message storage use the Pigeonhole and Dovecot worker in
`src/sieve`. Its frontend reads immutable snapshots, reserves Maildir
occurrences, verifies per-occurrence outcomes, and retains failed or interrupted
mail for recovery. Public commands call the installed frontend.

See [the compiler and workflow interface](../../docs/filter-interfaces.md) and
[Maildir delivery and recovery](../../docs/sieve-tools.md).
