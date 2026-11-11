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
The `cert` setting controls fetchmail and IMAP inspection; it cannot disable
SMTP verification.

IMAP inspection (jaro imap and automatic folder discovery) uses these connection
styles. The historical names retain their existing IMAP meaning:

  TLS1, tls, implicit             implicit TLS (usually port 993)
  SSL2, SSL23, SSL3, ssl, starttls STARTTLS (usually port 143)
  plain                          explicitly unencrypted IMAP

An omitted transport uses TLS1. Unknown values fail before connecting. The
historical SSL labels select STARTTLS, not obsolete SSL protocol versions;
OpenSSL negotiates the TLS version. Do not infer an IMAP style from the spelling
of a fetchmail sslproto option. These aliases describe inspection, not a change
to fetchmail's separate sslproto configuration or Mutt's peek settings.

For IMAP, cert check verifies both the chain and the DNS name (or IP address)
on the connection carrying authentication. It uses system trust unless a
readable certfile CA bundle is supplied. Verification and STARTTLS failures
are errors with no plaintext retry. The historical cert ignore default remains
an explicit, warned opt-out from IMAP verification; set cert check for verified
inspection. transport plain also warns because LOGIN is sent unencrypted.
IMAP uses OpenSSL s_client with hostname/IP verification support (1.1.1 or newer)
and nc only for explicit plain. JARO_IMAP_TIMEOUT bounds the entire connection
and query to 30 seconds by default; accepted values are 1..300 seconds. Timeout
and child failures return nonzero. LOGIN quotes and backslashes are escaped;
CR, LF and NUL in login/password values are rejected before connecting.

Folder discovery
----------------
jaro imap listfolders writes one exact folder name per line, including quoted
spaces, tabs, leading/trailing spaces, quotes and backslashes. It accepts IMAP4rev1
LIST responses with parenthesized flags, a quoted one-character hierarchy
delimiter or NIL, and an atom or quoted mailbox name. Quoted strings support
only the IMAP escapes for double quote and backslash. Empty quoted names are
retained. Names remain in their server encoding: 7-bit modified UTF-7 is passed
through without decoding. UTF-8/IMAP4rev2 negotiation is not implemented.

Literal strings (including {length} mailbox names), LIST extensions, unsupported
control/non-ASCII bytes and malformed responses are errors; no partial list is
published. Responses must contain successful tagged completions for CAPABILITY,
LOGIN, LIST and LOGOUT in order, and every line must end in LF (CRLF is accepted).
Tagged NO/BAD, early disconnects, missing completions and transport failures
return nonzero. Supported limits are 1 MiB per response, 16 KiB per line and
10,000 mailbox entries. These limits apply to parsing, alongside the transport
deadline above.

Without configured folders, fetch stops before invoking fetchmail if discovery
fails, returns no folders, or exclusions remove them all. Configured folders
bypass discovery. Discovered and configured names each become one quoted
fetchmail configuration token; spaces, tabs, quotes and backslashes retain their
boundaries. See https://www.fetchmail.info/fetchmail-man.html for the run
control file grammar and its hexadecimal string-delimiter escapes.

The old smtp_fingerprints.zkv cache is no longer used. Existing private or
self-signed servers must have their CA trusted by the system or by certfile.
See https://marlam.de/msmtp/msmtp.html#Transport-Layer-Security for msmtp TLS
configuration details.
