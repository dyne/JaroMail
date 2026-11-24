# abook query fixture

The first line is the Mutt query status. Each later line has the mailbox,
display name and notes separated by tabs. Query results preserve the order of
contacts and their mailboxes in the fixture. The missing notes field on the
second contact appears as a single space in the result.

The [query test](../../abook-query-test.py) checks the maintained fork by
default. Pass `--baseline` with a pristine 0.6.2 executable to check the
recorded upstream behavior.

## Search keys

Unicode folding matches `Zoe` with `Zoë O'Neil` and returns both original
mailboxes. The fork also distinguishes empty books from read failures and
matches individual fields, including each mailbox on its own.

## Installed consumers

[The installed test](../../test-abook-consumers.sh) uses the relocated JaroMail
wrapper and bundled abook. Its controlled NeoMutt config uses JaroMail's
generated `query_command` and installed Tab binding. The draft editor records
the resulting To header. The filter compiler driver reads the same books through
the production snapshot reader and checks both export lists and Sieve output.

| Case | `jaro -q complete` | NeoMutt Tab |
| --- | --- | --- |
| Contact | Blank status, multiword name and separate note | Selects the named mailbox |
| Group | Blank status followed by the group row, exit 0 | Selects the first group |
| Mixed | Two contacts in book order, then the group | All three selectable in wire order with client sorting disabled |
| Miss | `Not found` status only, exit 1 | Leaves the entered text |
| `group/roster` | Blank status followed by the group row, exit 0 | Selects the first group |
| Quoted mailbox | Preserves `"a b"@example.org` in the mailbox column | NeoMutt 20250510 selects `ab@example.org` |
| Alternate list | Default book misses, explicit list finds the contact | Generated default misses, explicit command override selects the contact |

`addr` reads the TAB-separated mailbox and name fields, including spaces in
quoted local parts, and omits notes. `isknown` and duplicate checks in `import`
compare exact normalized mailboxes, including secondary addresses. An address
written only in notes appears in completion queries and stays out of membership.
The native filter snapshot retains the quoted mailbox and excludes notes.

[The group suite](../../test-group-modes.sh) covers pseudo-addresses and
recipient counts. [The addressbook suite](../../test-addressbook-parse.sh)
covers allow/block list selection and ordinary known/unknown senders.
[The filter compiler suite](../../filter_compiler_test.py) already checks
quoted local parts in explicit rules against the Sieve interpreter.
