#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
source "$repo_root/src/zlibs/email"
source "$repo_root/src/zlibs/search"
source "$repo_root/src/zlibs/filters"
MAILDIRS="$tmp_root/mail"
mkdir -p "$MAILDIRS"
touch "$MAILDIRS/notmuch-config"
# This fixture exercises dispatcher errors with an existing explicit policy;
# fresh-template validation has separate installed-workflow coverage.
touch "$MAILDIRS/Filters.txt"

(
  account=fixture DRYRUN=0
  read_account() {
    proto=pop host=fixture.invalid port=995 password=fixture login=fixture
    accountopt=keep cert=check transport=TLS1 name=fixture imap=fixture.invalid
    folders=(INBOX)
  }
  nm_setup() { return 0; }
  is_online() { return 0; }
  fetchmail() {
    cat >/dev/null
    if [[ "$*" == *-c* ]]; then return $probe_status; fi
    print downloaded >> "$tmp_root/downloads"
    return $download_status
  }
  probe_status=0 download_status=9
  assert_status 9 fetch
  download_status=0
  assert_status 0 fetch
  for probe_status in 2 3 7 9; do
    assert_status "$probe_status" fetch
  done
  probe_status=1
  assert_status 0 fetch
  assert_equal "$(wc -l < "$tmp_root/downloads" | tr -d ' ')" 2 "failed and empty polls do not download"
  probe_status=0 DRYRUN=1
  assert_status 0 fetch
)
(
  ACCOUNTS="$tmp_root/accounts"
  mkdir -p "$ACCOUNTS"
  touch "$ACCOUNTS/first" "$ACCOUNTS/second"
  account=""
  fetch() { [[ "$account" == second ]]; }
  assert_status 1 fetchall
  assert_equal "$account" "" "account selection restored"
)
(
  fetch() { return 9; }
  update_filters() { print unexpected > "$tmp_root/updated"; }
  option_params=()
  exitcode=0
  cmd_fetch || true
  assert_equal "$exitcode" 9 "fetch command reports transport failure"
  [[ ! -e "$tmp_root/updated" ]]
)
(
  notmuch_run() { return 42; }
  assert_status 42 nm search fixture
  option_params=(fixture) subcommand=search
  save_replay() { cat; }
  cmd_search || true
  assert_equal "$exitcode" 42 "search pipeline reports producer failure"
  cmd_notmuch || true
  assert_equal "$exitcode" 42 "notmuch pipeline reports producer failure"
  read_account() { return 0; }
  newlock() { return 0; }
  unlock() { print released > "$tmp_root/index-released"; }
  assert_status 42 nm_index
  [[ -f "$tmp_root/index-released" ]]
)
(
  mkdir -p "$MAILDIRS/outbox/"{cur,new,tmp}
  print first > "$MAILDIRS/outbox/new/1"
  print second > "$MAILDIRS/outbox/new/2"
  account=fixture DRYRUN=0 global_quit=0
  lock() { return 0; }; unlock() { return 0; }
  hdr() { print 'From: fixture@example.org'; }
  zstat() { print 10; }; human_size() { print 10; }
  smtp_send() { [[ "$(cat)" == second ]] || return 9; }
  learn() { cat >/dev/null; }
  printfile() { cat "$1"; }
  deliver() { cat >/dev/null; }
  assert_status 9 send
  [[ -f "$MAILDIRS/outbox/new/1" && ! -e "$MAILDIRS/outbox/new/2" ]]
)

# Exercise the actual entrypoint and dispatcher with a failed notmuch process.
mkdir -p "$tmp_root/bin"
printf '%s\n' '#!/bin/sh' 'exit 42' > "$tmp_root/bin/notmuch"
chmod +x "$tmp_root/bin/notmuch"
export PATH="$tmp_root/bin:$PATH"
export JAROMAILDIR="$MAILDIRS" JAROWORKDIR="$repo_root/src"
assert_status 42 "$repo_root/src/jaro" notmuch search fixture > "$tmp_root/cli.log" 2>&1
assert_status 42 "$repo_root/src/jaro" search fixture >> "$tmp_root/cli.log" 2>&1
print -- 'PASS: transport, batch, index, pipeline and CLI exit statuses'
