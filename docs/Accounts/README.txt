Directory containing account information.

Accounts are your login information at email providers.

Each file in this directory configures a different account.

Accounts can be selected using the 'jaro -a accountname' option.

The account named default is the one used if none is specified.

SMTP requires msmtp 1.8.3 or newer and always verifies the certificate and
hostname on the sending connection, using the system CA store. For a private
CA, set `certfile /path/to/ca-bundle.pem` in the account configuration.
The `cert` setting controls fetchmail only; it cannot disable SMTP verification.

The old smtp_fingerprints.zkv cache is no longer used. Existing private or
self-signed servers must have their CA trusted by the system or by certfile.
See https://marlam.de/msmtp/msmtp.html#Transport-Layer-Security for msmtp TLS
configuration details.
