#!/usr/bin/env zsh

function j() {
	JAROMAILDIR=/tmp/jaromail-test \
			   JAROWORKDIR=/usr/local/share/jaromail \
			   jaro $*
}

function r() {
	print - "========================== $*"
}

# generate a maildir

function lorem() {
cat <<EOF
Lorem ipsum dolor sit amet

Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do
eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad
minim veniam, quis nostrud exercitation ullamco laboris nisi ut
aliquip ex ea commodo consequat. Duis aute irure dolor in
reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla
pariatur. Excepteur sint occaecat cupidatat non proident, sunt in
culpa qui officia deserunt mollit anim id est laborum.
EOF
}

j init

lorem | j compose fengi2Ee@dyne.org
lorem | j compose Juiv0air@dyne.org
lorem | j compose Ieshuem3@dyne.org

lorem | j compose fengi2Ee@riseup.net
lorem | j compose Juiv0air@riseup.net
lorem | j compose Ieshuem3@riseup.net

lorem | j compose fengi2Ee@autistici.org
lorem | j compose Juiv0air@autistici.org
lorem | j compose Ieshuem3@autistici.org

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

exported_recipients="$(j extract /tmp/jaromail-test/outbox 2>/dev/null | sort | uniq)"
if [[ "$exported_recipients" = "$exported_recipients_ok" ]]; then
	r "EXTRACT OK"
else
	r "EXTRACT ERROR"
	print "$exported_recipients"
	print - "--"
	print "$exported_recipients_ok"
fi

imported_sender="Luther Blisset <luther@dyne.org>"


print $imported_sender | j import
if [[ "$(j addr 2>/dev/null)" = "$imported_sender" ]]; then
	r "IMPORT OK"
else
	r "IMPORT ERROR"
fi

if j update &&
		j index &&
		j filter outbox; then
	r "UPDATE and INDEX and FILTER OK"
else
	return 1
fi

if j search to:juiv0air | jaro headers | grep 'Lorem_ipsum_dolor_sit_amet$'; then
	r "SEARCH and HEADERS OK"
else
	return 1
fi

print "Luther Blisset <luther@dyne.org>" | j import -l blacklist

j filter known

#TODO: here test hooks

rm -rf /tmp/jaromail-test
