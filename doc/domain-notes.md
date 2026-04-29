# Domain Notes

## Ubiquitous Language

- **Account**: one mail identity/config under `Accounts/`.
- **Maildir**: canonical local storage tree for messages.
- **Filter cache**: compiled routing model generated from `Filters.txt` + addressbooks.
- **Known/Blacklist/Whitelist**: contact trust classes driving routing and priority.
- **Replay**: persisted command output snapshot used by downstream commands.

## Invariants

- Maildir layout must provide: `incoming`, `known`, `sent`, `priv`, `postponed`, `drafts`, `unsorted`, `remember`, `outbox`.
- `update` must rebuild filter cache before routing-dependent flows.
- Addressbook semantics:
  - `whitelist.abook` and `blacklist.abook` are list-of-contact truth.
  - `-l <name>` switches active list scope.
- Filter order is deterministic in implementation:
  1. configured `from` rules,
  2. configured `to/cc` rules,
  3. whitelist,
  4. spam/header checks,
  5. blacklist,
  6. own-address private routing,
  7. `unsorted` fallback.
- Account discovery must resolve one active account before network/index actions.
- Offline-first default: local maildir operations work without active network access.

## Domain Events

- **message_queued**: message staged in `outbox`.
- **mail_fetched**: remote mail copied into `incoming`.
- **filters_updated**: cache regenerated from config/addressbooks.
- **message_delivered**: message injected/refiled to a destination maildir.
- **index_refreshed**: notmuch update/index completed.
- **address_learned**: sender/contacts added to active addressbook.
- **password_requested**: keyring/pass/pinentry retrieval path executed.

## Ports and Adapters

- **Ports (domain-facing)**:
  - search/index port,
  - address extraction port,
  - mutt UI launch port,
  - credential retrieval port.
- **Current adapters**:
  - `notmuch_run` in `src/zlibs/search`,
  - `mblaze_extract_addresses` in `src/zlibs/parse`,
  - keyring/pass/secret-tool/gnome-keyring branches in `src/zlibs/keyring`,
  - mutt invocation in `src/zlibs/mutt`.

## Test Cross-References

- address/parse invariants: `extras/test/test-addressbook-parse.sh`
- filtering/routing invariants: `extras/test/test-filtering.sh`
- helper binary boundary: `extras/test/test-helpers.sh`
- end-to-end source workflow: `extras/test/run-source.sh`
