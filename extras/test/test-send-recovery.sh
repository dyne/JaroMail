#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
source "$repo_root/src/zlibs/email"

# Build the compile-time fault variant through the real CMake target. No fault
# environment variable is interpreted by the installed production helper.
if ! python3 "$repo_root/build/cmake-build.py" --testing --target jaro-outbox-test > "$tmp_root/build.log" 2>&1; then
  cat "$tmp_root/build.log"
  exit 1
fi
helper="$repo_root/build/gnu/jaro-outbox-test"
outbox_state() { "$helper" "$1" "$MAILDIRS" "$2"; }
lock() { return 0; }; unlock() { return 0; }
hdr() { print -- 'From: fixture@example.org'; }
learn() { cat > /dev/null; }
warning() { print -r -- "$*" >> "$MAILDIRS/warnings"; }
DRYRUN=0 global_quit=0 account=fixture
cases=0

setup_case() {
  MAILDIRS="$tmp_root/$1"
  mkdir -p "$MAILDIRS/outbox/"{new,cur,tmp}
  printf 'From: fixture@example.org\r\nTo: private@example.org\r\nSubject: private\r\n\r\noriginal\000bytes\377\r\n' > "$MAILDIRS/original"
  cp "$MAILDIRS/original" "$MAILDIRS/outbox/new/message"
  smtp_status=0
  unset JARO_OUTBOX_FAULT JARO_OUTBOX_CRASH
}
smtp_send() {
  print -- called >> "$MAILDIRS/smtp-calls"
  cat > "$MAILDIRS/transport-input"
  return $smtp_status
}
calls() {
  if [[ -f "$MAILDIRS/smtp-calls" ]]; then
    wc -l < "$MAILDIRS/smtp-calls" | tr -d ' '
  else print 0; fi
}
record_id() { local report; report=$(outbox_state inspect new/message); print -- "${report%% *}"; }
pass_case() { (( cases += 1 )); print -- "PASS case: $1"; }
fault_reached() { grep -F -- "OUTBOX-TEST $1 reached: $2" "$MAILDIRS/fault.log"; }
retained() { cmp "$MAILDIRS/original" "$MAILDIRS/outbox/new/message"; }
archived() {
  local -a files
  files=("$MAILDIRS"/sent/{new,cur}/*(.DN))
  assert_equal "${#files}" 1 'one deterministic sent copy'
  cmp "$MAILDIRS/original" "$files[1]"
  [[ ! -e "$MAILDIRS/outbox/new/message" ]]
  tmp_files=("$MAILDIRS"/sent/tmp/*(.DN))
  assert_equal "${#tmp_files}" 0 'no remaining sent staging file'
}

setup_case success
send
assert_equal "$(calls)" 1 'first SMTP'
cmp "$MAILDIRS/original" "$MAILDIRS/transport-input"
archived
send
assert_equal "$(calls)" 1 'empty restart'
pass_case 'successful transport preserves exact bytes and archives once'

for fault in sent-file-sync sent-publish sent-directory-sync queue-unlink; do
  setup_case "local-$fault"
  export JARO_OUTBOX_FAULT="$fault"
  assert_status 1 send 2> "$MAILDIRS/fault.log"
  fault_reached fault "$fault"
  retained
  assert_equal "$(calls)" 1 "$fault reached after SMTP"
  id=$(record_id)
  assert_equal "$(cat "$MAILDIRS/.outbox-state/$id/state")" "$([[ $fault == queue-unlink ]] && print complete || print accepted)" 'acceptance retained'
  unset JARO_OUTBOX_FAULT
  send
  archived
  assert_equal "$(calls)" 1 'local recovery never repeats SMTP'
  pass_case "$fault local recovery"
done

setup_case moved-sent
export JARO_OUTBOX_FAULT=queue-unlink
assert_status 1 send
unset JARO_OUTBOX_FAULT
sent=("$MAILDIRS"/sent/new/*(.DN))
mv "$sent[1]" "$MAILDIRS/sent/cur/${sent[1]:t}:2,S"
send
archived
assert_equal "$(calls)" 1 'flagged sent copy reused'
pass_case 'sent copy moved to cur is reused'

for fault in directory-sync claim-file-sync claim-directory-sync record-claim-sync payload-file-sync identity-file-sync payload-directory-sync prepared-file-sync prepared-publish prepared-directory-sync queue-presend-sync inflight-file-sync inflight-directory-sync; do
  setup_case "write-$fault"
  export JARO_OUTBOX_FAULT="$fault"
  assert_status 1 send 2> "$MAILDIRS/fault.log"
  fault_reached fault "$fault"
  retained
  assert_equal "$(calls)" 0 'required persistence failure prevents SMTP'
  unset JARO_OUTBOX_FAULT
  pass_case "$fault prevents transport"
done

for fault in accepted-file-sync accepted-publish accepted-directory-sync complete-file-sync complete-directory-sync; do
  setup_case "accept-$fault"
  export JARO_OUTBOX_FAULT="$fault"
  assert_status 1 send 2> "$MAILDIRS/fault.log"
  fault_reached fault "$fault"
  retained
  assert_equal "$(calls)" 1 'acceptance fault follows transport'
  unset JARO_OUTBOX_FAULT
  send || true
  assert_equal "$(calls)" 1 'acceptance persistence fault never repeats SMTP'
  pass_case "$fault recovery"
done

# Each crash point is an exact post/pre-transition boundary in the test helper.
# The first invocation must hit it (nonzero status); re-entry may only transport
# a prepared item, or finish local work after a durable/visible acceptance.
for phase in claim-created payload-durable prepared-durable inflight-publish inflight-durable accepted-publish accepted-durable sent-publish sent-durable complete-durable queue-unlink queue-removed; do
  setup_case "crash-$phase"
  export JARO_OUTBOX_CRASH="$phase"
  if send 2> "$MAILDIRS/fault.log"; then print -u2 -- "crash phase not reached: $phase"; exit 1; fi
  fault_reached crash "$phase"
  first_calls=$(calls)
  if [[ $phase != queue-removed ]]; then retained; fi
  unset JARO_OUTBOX_CRASH
  send || true
  case "$phase" in
    prepared-durable|inflight-publish) expected=1 ;;
    *) expected=$first_calls ;;
  esac
  assert_equal "$(calls)" "$expected" "$phase restart SMTP count"
  if (( first_calls > 0 )); then
    local_payload=("$MAILDIRS"/.outbox-state/*/message(.DN))
    cmp "$MAILDIRS/original" "$local_payload[1]"
  fi
  pass_case "restart $phase"
done

setup_case uncertain
smtp_status=69
assert_status 69 send
retained
smtp_status=0
assert_status 1 send
assert_equal "$(calls)" 1 'all nonzero transport exits held'
assert_contains "$(cat "$MAILDIRS/warnings")" 'delivery uncertain' 'held state visible'
assert_contains "$(cat "$MAILDIRS/warnings")" '1 outbox item(s) held' 'held count visible'
pass_case 'ambiguous transport status held on re-entry'

setup_case preflight
functions[transport_stub]="$functions[smtp_send]"
smtp_send() { smtp_delivery_outcome=not-started; return 7; }
assert_status 7 send
assert_equal "$(calls)" 0 'preflight has no transport'
functions[smtp_send]="$functions[transport_stub]"
send
assert_equal "$(calls)" 1 'proven unstarted attempt eligible'
archived
pass_case 'local preflight failure is safely eligible'

setup_case accepted-cleanup-status
smtp_send() {
  print -- called >> "$MAILDIRS/smtp-calls"
  cat > "$MAILDIRS/transport-input"
  smtp_delivery_outcome=accepted
  return 8
}
assert_status 8 send
archived
send
assert_equal "$(calls)" 1 'accepted SMTP with local config cleanup failure never resent'
functions[smtp_send]="$functions[transport_stub]"
pass_case 'accepted SMTP survives a later local SMTP-config cleanup failure'

setup_case changed-during-transport
smtp_send() {
  print -- called >> "$MAILDIRS/smtp-calls"
  cat > "$MAILDIRS/transport-input"
  printf changed > "$MAILDIRS/outbox/new/message"
  return 0
}
assert_status 1 send
record_paths=("$MAILDIRS"/.outbox-state/*/state(.DN))
assert_equal "$(cat "$record_paths[1]")" accepted 'acceptance saved despite queue replacement'
assert_status 1 send
assert_equal "$(calls)" 1 'changed bytes after acceptance never transported'
payloads=("$MAILDIRS"/.outbox-state/*/message(.DN))
cmp "$MAILDIRS/original" "$payloads[1]"
functions[smtp_send]="$functions[transport_stub]"
pass_case 'queue changed during SMTP preserves acceptance and original snapshot'

setup_case moved-during-transport
smtp_send() {
  print -- called >> "$MAILDIRS/smtp-calls"
  cat > "$MAILDIRS/transport-input"
  mv "$MAILDIRS/outbox/new/message" "$MAILDIRS/outbox/cur/message:2,S"
  return 0
}
assert_status 1 send
functions[smtp_send]="$functions[transport_stub]"
send
archived
[[ ! -e "$MAILDIRS/outbox/cur/message:2,S" ]]
assert_equal "$(calls)" 1 'Maildir movement during transport cannot cause resend'
pass_case 'queue renamed during SMTP finishes locally on re-entry'

setup_case identical
cp "$MAILDIRS/outbox/new/message" "$MAILDIRS/outbox/new/second"
send
assert_equal "$(calls)" 2 'identical messages with different IDs both send'
assert_equal "$(find "$MAILDIRS/.outbox-state" -name state | wc -l | tr -d ' ')" 2 'distinct journal IDs'
pass_case 'stable queue identity separates identical payloads'

setup_case flagged-queue
outbox_state prepare new/message > "$MAILDIRS/report"
before_id="$(record_id)"
mv "$MAILDIRS/outbox/new/message" "$MAILDIRS/outbox/cur/message:2,S"
send
assert_equal "$(calls)" 1 'new-to-cur movement remains eligible once'
assert_equal "$(find "$MAILDIRS/.outbox-state" -name state | wc -l | tr -d ' ')" 1 'flags keep stable journal identity'
assert_equal "$(cat "$MAILDIRS/.outbox-state/$before_id/state")" complete 'same record completed'
pass_case 'Maildir movement and flag changes preserve queue identity'

setup_case unusual-name
mv "$MAILDIRS/outbox/new/message" "$MAILDIRS/outbox/new/space [glob] λ"$'\n''tail'
send
assert_equal "$(calls)" 1 'unusual queue basename sent once'
pass_case 'whitespace Unicode and newline queue identity'

setup_case changed
outbox_state prepare new/message > "$MAILDIRS/report"
id="$(record_id)"
printf altered > "$MAILDIRS/outbox/new/message"
assert_status 1 send
assert_equal "$(calls)" 0 'modified queue held'
cmp "$MAILDIRS/original" "$MAILDIRS/.outbox-state/$id/message"
pass_case 'content mismatch holds without overwriting snapshot'

setup_case changed-to-anonymous
outbox_state prepare new/message > "$MAILDIRS/report"
outbox_state inflight new/message
printf 'From: anon@mixmaster\nTo: one@example.org\n\nchanged\n' > "$MAILDIRS/outbox/new/message"
functions[normal_hdr]="$functions[hdr]"
hdr() { awk '/^$/ { exit } { print }' "$1"; }
mixmaster() { print unexpected > "$MAILDIRS/mixmaster-called"; }
assert_status 1 send
[[ ! -e "$MAILDIRS/mixmaster-called" ]]
assert_equal "$(calls)" 0 'From change cannot bypass SMTP hold'
pass_case 'anonymous From edit cannot bypass existing SMTP state'

setup_case anonymous
printf 'From: anon@mixmaster\nTo: one@example.org\nSubject: legacy\n\nanonymous body\n' > "$MAILDIRS/outbox/new/message"
cp "$MAILDIRS/outbox/new/message" "$MAILDIRS/original"
zmodload zsh/system
typeset -A e_addr
e_parse() { cat >/dev/null; e_addr[one@example.org]=one; }
ztmp() { ztmpfile=$(mktemp "$MAILDIRS/anonymous.XXXXXXXX"); }
mixmaster() { print called >> "$MAILDIRS/mixmaster-calls"; cat > "$MAILDIRS/anonymous-input"; }
printfile() { cat "$1"; }
deliver() { cat > "$MAILDIRS/anonymous-sent"; }
send
assert_equal "$(wc -l < "$MAILDIRS/mixmaster-calls" | tr -d ' ')" 1 'legacy Mixmaster invoked once'
assert_equal "$(calls)" 0 'legacy anonymous mail avoids SMTP recovery path'
[[ ! -e "$MAILDIRS/.outbox-state" && ! -e "$MAILDIRS/outbox/.recovery-claims" && ! -e "$MAILDIRS/outbox/new/message" ]]
cmp "$MAILDIRS/original" "$MAILDIRS/anonymous-sent"
functions[hdr]="$functions[normal_hdr]"
pass_case 'unjournaled anonymous transport remains compatible'

for damage in corrupt-state nul-state missing-state missing-record missing-journal corrupt-payload missing-claim; do
  setup_case "$damage"
  outbox_state prepare new/message > "$MAILDIRS/report"
  id="$(record_id)"
  outbox_state inflight new/message
  case "$damage" in
    corrupt-state) print broken > "$MAILDIRS/.outbox-state/$id/state" ;;
    nul-state) printf 'accepted\000corrupt\n' > "$MAILDIRS/.outbox-state/$id/state" ;;
    missing-state) rm "$MAILDIRS/.outbox-state/$id/state" ;;
    missing-record) rm -r "$MAILDIRS/.outbox-state/$id" ;;
    missing-journal) rm -r "$MAILDIRS/.outbox-state" ;;
    corrupt-payload) print changed > "$MAILDIRS/.outbox-state/$id/message" ;;
    missing-claim) rm "$MAILDIRS/outbox/.recovery-claims/$id" ;;
  esac
  assert_status 1 send
  retained
  assert_equal "$(calls)" 0 "$damage never becomes a fresh attempt"
  pass_case "$damage held"
done

setup_case dryrun
DRYRUN=1 send
assert_equal "$(calls)" 0 'dry run has no transport'
[[ ! -e "$MAILDIRS/.outbox-state" && ! -e "$MAILDIRS/outbox/.recovery-claims" ]]
retained
pass_case 'dry run creates no journal or claim'

for fault in verified-file-sync verified-publish verified-directory-sync acceptance-recovery-sync; do
  setup_case "verified-$fault"
  outbox_state prepare new/message > "$MAILDIRS/report"
  outbox_state inflight new/message
  outbox_state uncertain new/message
  export JARO_OUTBOX_FAULT="$fault"
  assert_status 1 outbox_state archive new/message 2> "$MAILDIRS/fault.log"
  fault_reached fault "$fault"
  retained
  assert_equal "$(calls)" 0 'archive fault never invokes SMTP'
  unset JARO_OUTBOX_FAULT
  outbox_state archive new/message
  archived
  assert_equal "$(calls)" 0 'verified local recovery never invokes SMTP'
  pass_case "$fault verified archive recovery"
done

setup_case verified-crash
outbox_state prepare new/message > "$MAILDIRS/report"
outbox_state inflight new/message
outbox_state uncertain new/message
export JARO_OUTBOX_CRASH=verified-durable
assert_status 99 outbox_state archive new/message 2> "$MAILDIRS/fault.log"
fault_reached crash verified-durable
retained
unset JARO_OUTBOX_CRASH
send
archived
assert_equal "$(calls)" 0 'ordinary send finishes durable external verification without SMTP'
pass_case 'restart after durable external verification'

setup_case production-hooks
helper="$repo_root/build/gnu/jaro-outbox"
export JARO_OUTBOX_FAULT=inflight-file-sync JARO_OUTBOX_CRASH=accepted-durable
send
archived
assert_equal "$(calls)" 1 'production helper ignores test-only hooks'
unset JARO_OUTBOX_FAULT JARO_OUTBOX_CRASH
pass_case 'production binary has no fault environment controls'

print -- "Recovery cases: $cases passed, 0 failed, 0 skipped"
