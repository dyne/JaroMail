#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

missing_cmds=()
for req in pinentry fetchmail gpg msmtp notmuch abook; do
  command -v "${req}" >/dev/null 2>&1 || missing_cmds+=("${req}")
done
if (( ${#missing_cmds[@]} > 0 )); then
  print -- "SKIP test-addressbook-parse.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 0
fi

queue_mail() {
  recipient="${1}"
  sender="${2}"
  cat <<EOF | jaro_source queue "${recipient}"
From: ${sender}
To: ${recipient}
Cc: Team <team@example.org>
Subject: Parse_contract
Date: Thu, 01 Jan 1970 00:00:00 +0000

$(lorem_message)
EOF
}

jaro_source init
queue_mail "alpha@example.org" "Alice Example <alice@example.org>"
queue_mail "ALPHA@example.org" "Alice Example <alice@example.org>"
queue_mail "gamma@example.org" "Carol Example <carol@example.org>"

extracted="$(
  cat <<'EOF' | jaro_source extract stdin | sort
Date: Thu, 01 Jan 1970 00:00:00 +0000
From: Alpha Person <alpha@example.org>
Subject: one
JAROMAIL_PIPE_SEPARATOR
Date: Thu, 01 Jan 1970 00:00:01 +0000
From: ALPHA Person <ALPHA@example.org>
Subject: two
JAROMAIL_PIPE_SEPARATOR
Date: Thu, 01 Jan 1970 00:00:02 +0000
From: Gamma Person <gamma@example.org>
Subject: three
EOF
)"
assert_contains "${extracted}" "alpha@example.org" "extract includes alpha"
assert_contains "${extracted}" "gamma@example.org" "extract includes gamma"

alpha_count="$(print -- "${extracted}" | grep -ic "alpha@example.org")"
assert_equal "${alpha_count}" "1" "extract deduplicates case variants"

import_line="Known Person <known@example.org>"
print -- "${import_line}" | jaro_source import >/dev/null
assert_contains "$(jaro_source addr 2>/dev/null)" "${import_line}" "import into whitelist"

print -- "Blocked Person <blocked@example.org>" | jaro_source -l blacklist import >/dev/null
assert_contains "$(jaro_source -l blacklist addr 2>/dev/null)" "Blocked Person <blocked@example.org>" "import into blacklist"

maddr_shim="${test_bin}/maddr"
cat > "${maddr_shim}" <<'EOF'
#!/usr/bin/env sh
grep -i '^From:' \
  | sed -n 's/.*<\([^>]*\)>.*/\1/p' \
  | tr '[:upper:]' '[:lower:]'
EOF
chmod +x "${maddr_shim}"

known_mail="$(mktemp "${tmp_root}/known-mail.XXXX")"
cat > "${known_mail}" <<'EOF'
From: Known Person <known@example.org>
To: reader@example.org
Subject: known

body
EOF
if cat "${known_mail}" | jaro_source isknown >/dev/null 2>&1; then
  true
else
  print -- "ASSERT FAIL (isknown success): expected known sender"
  exit 1
fi

unknown_mail="$(mktemp "${tmp_root}/unknown-mail.XXXX")"
cat > "${unknown_mail}" <<'EOF'
From: Stranger <stranger@example.org>
To: reader@example.org
Subject: unknown

body
EOF
if cat "${unknown_mail}" | jaro_source isknown >/dev/null 2>&1; then
  print -- "ASSERT FAIL (isknown failure): expected unknown sender to fail"
  exit 1
fi

headers="$(find "${mail_root}/outbox" -type f | sort | head -n 1 | jaro_source headers)"
assert_contains "${headers}" "Parse_contract" "headers includes subject"
assert_contains "${headers}" ":outbox:" "headers includes folder"
