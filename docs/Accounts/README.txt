Directory containing account information.

Accounts are your login information at email providers.

Each file in this directory configures a different account.

Accounts can be selected using the 'jaro -a accountname' option.

The account named default is the one used if none is specified.

Account file grammar
--------------------
An account file is data, not an executable shell configuration. Each line is
a directive followed by a space or tab and its value. Blank lines and lines
starting with # (after optional whitespace) are ignored. An unquoted # at the
start of a value or after unquoted whitespace begins an inline comment.
To include a literal # after a space, quote it.

Supported directives are name, email, login, proto, imap, smtp, host, port,
imap_port, smtp_port, transport, certfile, auth, cert, password, options,
folders, exclude and my_hdr. Unknown directives are errors. Repeated directives
replace the earlier value, except my_hdr, whose lines are accumulated verbatim
for Mutt. Do not put shell commands in account files.

Scalar values may contain spaces. Single or double quotes group literal text;
the grouping quotes are removed. A backslash quotes the next character except
inside single quotes, where it remains literal. Unquoted leading and trailing
whitespace is ignored; quoted whitespace is retained. folders and exclude are
lists separated by unquoted spaces or tabs. For example:

name "Alice O'Connor"
folders INBOX "Work projects" 'Archive 2025' "a\\b" "a\"b"
password 'literal $HOME, $(command), `command`, * and backslash: \'

No variables, command substitutions, arithmetic, wildcards, tilde expansion or
shell expressions are evaluated. Previously executable configuration is
intentionally unsupported: replace expressions with their literal values.
Unclosed quotes and a final unquoted backslash produce a file/line error.
Diagnostics identify the directive without displaying its password value.

proto accepts imap (default) or pop. Hosts must be DNS/IPv4 names or IPv6
literals, without whitespace or shell punctuation. Ports must be decimal
integers from 1 through 65535. The old host directive supplies imap and smtp
when absent; imap supplies host. port supplies imap_port, and an explicit
imap_port supplies smtp_port when absent. Otherwise the port defaults are
993 and 587. Other defaults remain TLS1, auth plain, cert ignore and options
keep. Each read starts with empty account state, including password and lists.
An explicit SMTP batch may reuse its password only for the same account and
unchanged SMTP identity; selecting another account discards that cached value.

Wizard output
-------------
Run jaro wizard and type display names and logins literally, without adding
account-file quotes yourself. The wizard escapes quotes/backslashes and quotes
values when necessary; the same writer preserves spaces and special characters
in list entries. Its generated From header also escapes Mutt variables and
backticks; manually written my_hdr lines retain their Mutt semantics.
Ports and hosts use the parser's validation before any optional
reachability check. Port errors name the field and allowed range; malformed
account files name the directive and source line, never the password value.

The wizard creates a private (0600) staging file in Accounts, parses it with
the same account reader, then publishes it using the POSIX link utility. This
requires the standard link and mktemp utilities and hard-link support in the
account directory. No existing file, directory or symlink is overwritten,
including an existing account with the .txt suffix. Invalid input, incomplete
scripted input or failed validation/publication leaves no new account file;
the private staging file is removed. Existing account files are not rewritten.
Carriage returns and newlines cannot occur inside a serialized field value.

SMTP requires msmtp 1.8.3 or newer and always verifies the certificate and
hostname on the sending connection, using the system CA store. For a private
CA, set `certfile /path/to/ca-bundle.pem` in the account configuration.
The `cert` setting controls fetchmail only; it cannot disable SMTP verification.

The old smtp_fingerprints.zkv cache is no longer used. Existing private or
self-signed servers must have their CA trusted by the system or by certfile.
See https://marlam.de/msmtp/msmtp.html#Transport-Layer-Security for msmtp TLS
configuration details.
