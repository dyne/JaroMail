#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
source "$repo_root/src/zlibs/email"
source "$repo_root/src/zlibs/locking"
outbox_state() { "$repo_root/build/gnu/jaro-outbox" "$1" "$MAILDIRS" "$2"; }
owned_children=()
release_paths=()
cleanup_children() {
  local prior=$? child attempt
  set +e
  for release_path in "${release_paths[@]}"; do touch "$release_path"; done
  for child in "${owned_children[@]}"; do
    if kill -0 "$child" 2>/dev/null; then kill -TERM "$child" 2>/dev/null; fi
    for attempt in {1..100}; do
      kill -0 "$child" 2>/dev/null || break
      sleep 0.01
    done
    if kill -0 "$child" 2>/dev/null; then kill -KILL "$child" 2>/dev/null; fi
    wait "$child" 2>/dev/null
  done
  test_cleanup
  return $prior
}
trap cleanup_children EXIT
await_exit() {
  local child="$1" attempt
  for attempt in {1..200}; do
    kill -0 "$child" 2>/dev/null || return 0
    sleep 0.01
  done
  print -u2 -- "owned child did not exit within bounded wait: $child"
  return 1
}
MAILDIRS="$tmp_root/mail"
WORKDIR="$tmp_root/runtime"
mkdir -p "$MAILDIRS/outbox/"{new,cur,tmp} "$WORKDIR/bin"
cat > "$WORKDIR/bin/dotlock" <<'EOF'
#!/bin/sh
if [ "$1" = -r ]; then shift 2; fi
if [ "$1" = -u ]; then
  rmdir "$2.lock"
else
  mkdir "$1.lock" 2>/dev/null || exit 3
fi
EOF
chmod +x "$WORKDIR/bin/dotlock"
helper.isfound() { return 1; }
hdr() { print -- 'From: sender@example.org'; }
zstat() { print 10; }
human_size() { print 10; }
smtp_send() { print -- called >> "$tmp_root/smtp-calls"; return 9; }
DRYRUN=0 global_quit=0 account=fixture

print message > "$MAILDIRS/outbox/new/message"
print partial > "$MAILDIRS/outbox/tmp/incomplete"
# A contending sender neither sends nor changes another owner's marker.
mkdir "$MAILDIRS/outbox.lock"
print 12345 > "$MAILDIRS/outbox.pid"
if send; then print -u2 -- 'contended send succeeded'; exit 1; fi
[[ ! -e "$tmp_root/smtp-calls" ]]
assert_equal "$(cat "$MAILDIRS/outbox.pid")" 12345 "owner retained"
if unlock "$MAILDIRS/outbox"; then print -u2 -- 'foreign lock released'; exit 1; fi
[[ -d "$MAILDIRS/outbox.lock" ]]
rm "$MAILDIRS/outbox.pid"
rmdir "$MAILDIRS/outbox.lock"

# SMTP failure releases our lock, and tmp/ entries are never sent.
if send; then print -u2 -- 'failed SMTP accepted'; exit 1; fi
assert_equal "$(wc -l < "$tmp_root/smtp-calls" | tr -d ' ')" 1 "only complete message sent"
[[ ! -e "$MAILDIRS/outbox.lock" && ! -e "$MAILDIRS/outbox.pid" ]]
assert_equal "$smtp_keep_password" 0 "password reuse disabled"
[[ -z "${password-}" ]]

# The queue is read after lock acquisition, not from a stale snapshot.
functions[real_lock]="$functions[lock]"
lock() { real_lock "$@" || return $?; rm "$MAILDIRS/outbox/new/message"; }
send
assert_equal "$(wc -l < "$tmp_root/smtp-calls" | tr -d ' ')" 1 "empty queue after acquisition"
[[ ! -e "$MAILDIRS/outbox.lock" && ! -e "$MAILDIRS/outbox.pid" ]]
functions[lock]="$functions[real_lock]"

# Interrupt a real child sender and wait with a bounded deadline.
cat > "$tmp_root/interrupt.zsh" <<'EOF'
source "$1/src/zlibs/email"
fn() { :; }; notice() { :; }; error() { :; }; func() { :; }; act() { :; }
warning() { :; }
MAILDIRS="$2"; DRYRUN=0; global_quit=0; account=fixture; send_signal="${3:-TERM}"
outbox_helper="$1/build/gnu/jaro-outbox"
outbox_state() { "$outbox_helper" "$1" "$MAILDIRS" "$2"; }
lock() { print locked > "$MAILDIRS/held"; }
unlock() { rm "$MAILDIRS/held"; print released > "$MAILDIRS/released"; }
hdr() { print 'From: sender@example.org'; }
zstat() { print 10; }; human_size() { print 10; }
smtp_send() { kill -"$send_signal" $$; return 9; }
send
EOF
print message > "$MAILDIRS/outbox/new/signal-message"
zsh "$tmp_root/interrupt.zsh" "$repo_root" "$MAILDIRS" > "$tmp_root/child.log" 2>&1 &
sender_pid=$!
owned_children+=("$sender_pid")
for attempt in {1..100}; do
  [[ -f "$MAILDIRS/released" ]] && break
  sleep 0.02
done
if [[ ! -f "$MAILDIRS/released" ]]; then
  print -u2 -- 'interrupted sender did not release lock'; exit 1
fi
await_exit "$sender_pid"
if wait "$sender_pid"; then
  print -u2 -- 'interrupted sender returned success'; exit 1
fi
owned_children=()
[[ ! -e "$MAILDIRS/held" ]]
# Signal interruption persists an uncertain state and ordinary re-entry holds it.
assert_equal "$(find "$MAILDIRS/.outbox-state" -name state -exec cat {} \; | sort -u)" uncertain "signal held"
before_calls="$(wc -l < "$tmp_root/smtp-calls" | tr -d ' ')"
if send; then print -u2 -- 'uncertain signal item resent'; exit 1; fi
assert_equal "$(wc -l < "$tmp_root/smtp-calls" | tr -d ' ')" "$before_calls" "signal restart has no SMTP"
for send_signal in HUP INT; do
  rm "$MAILDIRS/released"
  print message > "$MAILDIRS/outbox/new/signal-$send_signal"
  zsh "$tmp_root/interrupt.zsh" "$repo_root" "$MAILDIRS" "$send_signal" > "$tmp_root/child-$send_signal.log" 2>&1 &
  sender_pid=$!
  owned_children+=("$sender_pid")
  for attempt in {1..100}; do
    [[ -f "$MAILDIRS/released" ]] && break
    sleep 0.02
  done
  [[ -f "$MAILDIRS/released" ]]
  await_exit "$sender_pid"
  if wait "$sender_pid"; then print -u2 -- "$send_signal sender returned success"; exit 1; else signal_status=$?; fi
  owned_children=()
  expected_status=129
  [[ $send_signal == INT ]] && expected_status=130
  assert_equal "$signal_status" "$expected_status" "$send_signal propagated"
  [[ ! -e "$MAILDIRS/held" ]]
  print -- "PRECONDITION/CLEANUP: $send_signal delivered in transport; sender reaped; local lock released"
done

# Two actual senders contend while the owner is held at the SMTP boundary.
# The first child's gate must be observed before starting the second child.
MAILDIRS="$tmp_root/concurrent"
mkdir -p "$MAILDIRS/outbox/"{new,cur,tmp}
print 'original concurrent bytes' > "$MAILDIRS/outbox/new/concurrent"
cp "$MAILDIRS/outbox/new/concurrent" "$MAILDIRS/original"
cat > "$tmp_root/concurrent.zsh" <<'EOF'
source "$1/src/zlibs/email"
source "$1/src/zlibs/locking"
fn() { :; }; notice() { :; }; error() { :; }; func() { :; }; act() { :; }; warning() { :; }
helper.isfound() { return 1; }
outbox_helper="$1/build/gnu/jaro-outbox"
outbox_state() { "$outbox_helper" "$1" "$MAILDIRS" "$2"; }
MAILDIRS="$2" WORKDIR="$3" DRYRUN=0 global_quit=0 account=fixture
hdr() { print 'From: fixture@example.org'; }
learn() { cat >/dev/null; }
smtp_send() {
  print called >> "$MAILDIRS/smtp-calls"
  cat > "$MAILDIRS/transport-input"
  touch "$MAILDIRS/entered"
  for attempt in {1..500}; do
    [[ -e "$MAILDIRS/release" ]] && return 0
    sleep 0.01
  done
  return 98
}
send
EOF
release_paths+=("$MAILDIRS/release")
zsh "$tmp_root/concurrent.zsh" "$repo_root" "$MAILDIRS" "$WORKDIR" > "$tmp_root/owner.log" 2>&1 &
owner_pid=$!
owned_children+=("$owner_pid")
for attempt in {1..100}; do
  [[ -f "$MAILDIRS/entered" ]] && break
  sleep 0.02
done
[[ -f "$MAILDIRS/entered" && -d "$MAILDIRS/outbox.lock" ]]
print -- 'PRECONDITION: owner entered SMTP; exclusive outbox lock held'
if zsh "$tmp_root/concurrent.zsh" "$repo_root" "$MAILDIRS" "$WORKDIR" > "$tmp_root/contender.log" 2>&1; then
  print -u2 -- 'concurrent sender acquired owner lock'; exit 1
else contender_status=$?; fi
assert_equal "$contender_status" 3 'concurrent sender fails at lock'
assert_equal "$(wc -l < "$MAILDIRS/smtp-calls" | tr -d ' ')" 1 'only owner invokes SMTP'
cmp "$MAILDIRS/outbox/new/concurrent" "$MAILDIRS/original"
touch "$MAILDIRS/release"
for attempt in {1..100}; do
  [[ ! -e "$MAILDIRS/outbox.lock" ]] && break
  sleep 0.02
done
[[ ! -e "$MAILDIRS/outbox.lock" ]]
await_exit "$owner_pid"
wait "$owner_pid"
owned_children=()
[[ ! -e "$MAILDIRS/outbox/new/concurrent" && ! -e "$MAILDIRS/outbox.pid" ]]
cmp "$MAILDIRS/transport-input" "$MAILDIRS/original"
sent=("$MAILDIRS"/sent/new/*(.DN))
assert_equal "${#sent}" 1 'one concurrent sent copy'
cmp "$sent[1]" "$MAILDIRS/original"
print -- 'CLEANUP: SMTP gate released; owner and contender reaped; 0 owned children; 0 held locks'
print -- 'PASS: exclusive send, ownership checks, failure, signal and concurrent cleanup'
