# abook 0.6.2 query fixture

The first line is the Mutt query status. Each later line has the mailbox,
display name and notes separated by tabs. Query results preserve the order of
contacts and their mailboxes in the fixture. The missing notes field on the
second contact appears as a single space in the result.

The [query test](../../abook-query-test.py) captures 0.6.2 behavior.

## Planned search correction

The planned Unicode search uses accent folding. A query for `Zoe` will return
both mailboxes for `Zoë O'Neil` with the original display name. Pin that result
in a separate replacement expectation when the new search is implemented.

## Installed consumers

[The installed test](../../test-abook-consumers.sh) uses the relocated JaroMail
wrapper and bundled abook. Its controlled NeoMutt config uses JaroMail's
generated `query_command` and installed Tab binding. The draft editor records
the resulting To header. The filter compiler driver reads the same books through
the production snapshot reader and checks both export lists and Sieve output.

| Case | `jaro -q complete` | NeoMutt Tab |
| --- | --- | --- |
| Contact | Blank status, multiword name and separate note | Selects the named mailbox |
| Group | `Not found` status preceding the group row, exit 0 | Selects the first group |
| Mixed | Two contacts in book order, then the group | All three selectable in wire order with client sorting disabled |
| Miss | `Not found` status only, exit 1 | Leaves the entered text |
| `group/roster` | Group row in the status position, exit 0 | Leaves the entered text |
| Quoted mailbox | Preserves `"a b"@example.org` in the mailbox column | NeoMutt 20250510 selects `ab@example.org` |
| Alternate list | Default book misses, explicit list finds the contact | Generated default misses, explicit command override selects the contact |

L3 owns the group status framing and mailbox boundary repairs. It should
preserve the first candidate for `group/`, emit a success status for a
group-only hit, and address the quoted-mailbox handoff. The current `addr`
consumer includes notes in the display name and splits the quoted local part
at its space. L3 should preserve the separate fields. The native filter
snapshot retains the quoted mailbox and excludes the mailbox-shaped note from
membership. Those filter expectations carry forward to the replacement.

[The group suite](../../test-group-modes.sh) covers pseudo-addresses and
recipient counts. [The addressbook suite](../../test-addressbook-parse.sh)
covers allow/block list selection and ordinary known/unknown senders.
[The filter compiler suite](../../filter_compiler_test.py) already checks
quoted local parts in explicit rules against the Sieve interpreter.
