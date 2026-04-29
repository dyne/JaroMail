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
for req in pinentry fetchmail gpg msmtp notmuch maddr; do
  command -v "${req}" >/dev/null 2>&1 || missing_cmds+=("${req}")
done
if ! command -v mutt >/dev/null 2>&1 && ! command -v neomutt >/dev/null 2>&1; then
  missing_cmds+=("mutt-or-neomutt")
fi
if (( ${#missing_cmds[@]} > 0 )); then
  print -- "SKIP run-source.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 0
fi

jaro_source init

lorem_message | jaro_source compose fengi2Ee@dyne.org
lorem_message | jaro_source compose Juiv0air@dyne.org
lorem_message | jaro_source compose Ieshuem3@dyne.org

lorem_message | jaro_source compose fengi2Ee@riseup.net
lorem_message | jaro_source compose Juiv0air@riseup.net
lorem_message | jaro_source compose Ieshuem3@riseup.net

lorem_message | jaro_source compose fengi2Ee@autistici.org
lorem_message | jaro_source compose Juiv0air@autistici.org
lorem_message | jaro_source -D compose Ieshuem3@autistici.org

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

exported_recipients="$(jaro_source extract "${mail_root}/outbox" 2>/dev/null | sort | uniq)"
if assert_equal "${exported_recipients}" "${exported_recipients_ok}" "extract recipients"; then
  r "EXTRACT OK"
else
  exit 1
fi

imported_sender="Luther Blisset <luther@dyne.org>"
print -- "${imported_sender}" | jaro_source import
if assert_equal "$(jaro_source addr 2>/dev/null)" "${imported_sender}" "import sender"; then
  r "IMPORT OK"
else
  exit 1
fi

if jaro_source update && jaro_source index && jaro_source filter outbox; then
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
jaro_source filter known
