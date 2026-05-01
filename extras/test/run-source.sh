#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

r() {
  print - "========================== $*"
}

missing_cmds=()
for req in pinentry fetchmail gpg msmtp notmuch; do
  command -v "${req}" >/dev/null 2>&1 || missing_cmds+=("${req}")
done
if (( ${#missing_cmds[@]} > 0 )); then
  print -- "SKIP run-source.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 0
fi

queue_lorem() {
  recipient="${1}"
  cat <<EOF | jaro_source queue "${recipient}"
From: Luther Blisset <luther@dyne.org>
To: ${recipient}
Subject: Lorem_ipsum_dolor_sit_amet
Date: Thu, 01 Jan 1970 00:00:00 +0000

$(lorem_message)
EOF
}

jaro_source init

queue_lorem fengi2Ee@dyne.org
queue_lorem Juiv0air@dyne.org
queue_lorem Ieshuem3@dyne.org

queue_lorem fengi2Ee@riseup.net
queue_lorem Juiv0air@riseup.net
queue_lorem Ieshuem3@riseup.net

queue_lorem fengi2Ee@autistici.org
queue_lorem Juiv0air@autistici.org
queue_lorem Ieshuem3@autistici.org

exported_recipients_ok="fengi2Ee <fengi2ee@autistici.org>
fengi2Ee <fengi2ee@dyne.org>
fengi2Ee <fengi2ee@riseup.net>
Ieshuem3 <ieshuem3@autistici.org>
Ieshuem3 <ieshuem3@dyne.org>
Ieshuem3 <ieshuem3@riseup.net>
Juiv0air <juiv0air@autistici.org>
Juiv0air <juiv0air@dyne.org>
Juiv0air <juiv0air@riseup.net>
Luther Blisset <luther@dyne.org>"

exported_recipients="$(
  find "${mail_root}/outbox" -type f -print0 \
    | while IFS= read -r -d '' mailfile; do
        hdr="${repo_root}/build/gnu/fetchaddr -a"
        /bin/cat "${mailfile}" | ${=hdr}
      done \
    | awk -F, '
      NF >= 2 {
        email=tolower($1)
        name=$2
        if (email ~ /^[^ @]+@[^ @]+$/) print name " <" email ">"
      }' \
    | sort -f | uniq
)"
if assert_equal "${exported_recipients}" "${exported_recipients_ok}" "extract recipients"; then
  r "EXTRACT OK"
else
  exit 1
fi

imported_sender="Luther Blisset <luther@dyne.org>"
print -- "${imported_sender}" | jaro_source import
if assert_contains "$(jaro_source addr 2>/dev/null)" "${imported_sender}" "import sender"; then
  r "IMPORT OK"
else
  exit 1
fi

if jaro_source update && jaro_source index && jaro_source filter "${mail_root}/outbox" && jaro_source index; then
  r "UPDATE and INDEX and FILTER OK"
else
  exit 1
fi

if jaro_source search to:juiv0air | jaro_source headers | grep 'Lorem_ipsum_dolor_sit_amet$'; then
  r "SEARCH and HEADERS OK"
else
  exit 1
fi

print -- "Luther Blisset <luther@dyne.org>" | jaro_source import -l blacklist
jaro_source filter "${mail_root}/known"
