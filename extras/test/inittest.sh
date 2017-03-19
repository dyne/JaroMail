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
Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.

Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur magni dolores eos qui ratione voluptatem sequi nesciunt. Neque porro quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci velit, sed quia non numquam eius modi tempora incidunt ut labore et dolore magnam aliquam quaerat voluptatem. Ut enim ad minima veniam, quis nostrum exercitationem ullam corporis suscipit laboriosam, nisi ut aliquid ex ea commodi consequatur? Quis autem vel eum iure reprehenderit qui in ea voluptate velit esse quam nihil molestiae consequatur, vel illum qui dolorem eum fugiat quo voluptas nulla pariatur?

At vero eos et accusamus et iusto odio dignissimos ducimus qui blanditiis praesentium voluptatum deleniti atque corrupti quos dolores et quas molestias excepturi sint occaecati cupiditate non provident, similique sunt in culpa qui officia deserunt mollitia animi, id est laborum et dolorum fuga. Et harum quidem rerum facilis est et expedita distinctio. Nam libero tempore, cum soluta nobis est eligendi optio cumque nihil impedit quo minus id quod maxime placeat facere possimus, omnis voluptas assumenda est, omnis dolor repellendus. Temporibus autem quibusdam et aut officiis debitis aut rerum necessitatibus saepe eveniet ut et voluptates repudiandae sint et molestiae non recusandae. Itaque earum rerum hic tenetur a sapiente delectus, ut aut reiciendis voluptatibus maiores alias consequatur aut perferendis doloribus asperiores repellat.
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
