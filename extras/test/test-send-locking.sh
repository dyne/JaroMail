#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
source "$repo_root/src/zlibs/email"
source "$repo_root/src/zlibs/locking"
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
MAILDIRS="$2"; DRYRUN=0; global_quit=0; account=fixture
lock() { print locked > "$MAILDIRS/held"; }
unlock() { rm "$MAILDIRS/held"; print released > "$MAILDIRS/released"; }
hdr() { print 'From: sender@example.org'; }
zstat() { print 10; }; human_size() { print 10; }
smtp_send() { kill -TERM $$; }
send
EOF
print message > "$MAILDIRS/outbox/new/message"
zsh "$tmp_root/interrupt.zsh" "$repo_root" "$MAILDIRS" > "$tmp_root/child.log" 2>&1 &
sender_pid=$!
for attempt in {1..100}; do
  [[ -f "$MAILDIRS/released" ]] && break
  sleep 0.02
done
if [[ ! -f "$MAILDIRS/released" ]]; then
  kill -TERM "$sender_pid" 2>/dev/null || true
  wait "$sender_pid" || true
  print -u2 -- 'interrupted sender did not release lock'; exit 1
fi
if wait "$sender_pid"; then
  print -u2 -- 'interrupted sender returned success'; exit 1
fi
[[ ! -e "$MAILDIRS/held" ]]
print -- 'PASS: exclusive send, ownership checks, failure and signal cleanup'
