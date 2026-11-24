# Contact storage

The storage interface uses C and the pinned SQLite amalgamation. Write
transactions are explicit. Each operation finalizes its statements and a failed
contact save rolls back its savepoint. The same interface accepts length-aware
editor text and legacy records. The C interface keeps ownership local to the
connection or the current operation.

Open existing files with read-only or read-write access. The store checks
ownership, private permissions, application identity, and schema version. A
caller may create an unpublished file exclusively in a trusted directory.
New files use mode 0600. Connections verify foreign keys, DELETE journaling
and EXTRA synchronous mode. SQLite creates a private rollback journal when
needed. Use a consistent SQLite backup for offline restoration and keep the
restored file private.

Books and contacts have stable IDs. Contact and address ordinals preserve
their order. Each edit checks the observed revision. Successful book and
contact changes advance a generation counter in the same write transaction.
Differently spelled and repeated addresses remain separate rows. Exact lookup
uses the existing minimal quoting and ASCII case comparison. It uses an index
over book ID and canonical mailbox. Contiguous search uses separate Unicode
keys for names, nicknames, notes and addresses.

An editor can keep an owned copy of a contact and close the read connection
while the user is typing. Save and delete compare the draft's book, ID and
revision inside a short write transaction. A conflict leaves the draft
available for review against the current contact. SQLite waits at most one
second per busy operation before returning a retryable status. A failed
commit rolls back, so a busy draft can be submitted again with the same
expected revision. Learning and address-list imports check the normalized
mailbox in the selected list within the transaction that adds it. A repeat
leaves the existing name, addresses and revision intact.

Dry runs use a read-only connection when the store exists. Before first use,
they check migration sources in a private in-memory database. The preview
creates no profile files and does not create a list or contact.

Names, nicknames, notes and custom fields retain valid UTF-8 bytes, including
tabs and newlines. A field can contain up to 64 KiB and a record can contain up
to 16 MiB of text across all fields. Each contact can hold 128 custom text
fields and 16,384 addresses. Mailboxes use the existing single-mailbox grammar
and reject controls. Mutt rows keep original mailbox spelling and replace
display controls with spaces.

First use checks the existing database identity, schema, and integrity. A
missing destination triggers one scan of the profile's legacy `.abook` names.
Sources must be readable regular files. Their contents and names are left in
place. Bytewise filename order determines import order, including when
`whitelist.abook` and `allow.abook` merge into `allow`, or when the two block
files merge into `block`. The historical SQLite file `Addressbook` remains
outside this import.

Initialization holds the private profile lock through publication. It takes
a shared lock on each source while reading it and verifies its identity and
content again before publication. It verifies count and content for each book
against the staged transaction, then reopens the private sibling database for
an integrity check. A no-overwrite link publishes the finished file. Directory sync
makes the new name durable before the temporary link is removed. An
interrupted run can discard an unpublished temporary or finish syncing a
published database. Source or storage failures leave legacy files available
for retry.
