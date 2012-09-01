## mix.cfg - Mixmaster configuration file
## see mixmaster(1) for a description
##
## All paths relative to compile-time defined SPOOL (default: ~/Mix)
## Can be overriden by environment variable $MIXPATH 
##
## $Id: mix.cfg.ex,v 1.4 2003/09/03 16:46:04 packbart Exp $

####################### Remailer configuration: ###########################

## Enable remailer functionality
REMAIL              y

SHORTNAME           foo
REMAILERNAME        Anonymous Remailer
REMAILERADDR        mix@example.net
ANONNAME            Anonymous
#ANONADDR            nobody@example.net
#COMPLAINTS          abuse@example.net

## Additional capstring flags (e.g.: testing filter mon)
#EXTFLAGS           testing

## Act as an intermediate hop only, forward anonymized messages to
## another remailer
MIDDLEMAN           n

## Supported formats:
MIX                 y
PGP                 n
UNENCRYPTED         n

## Only disable these if you really know what they do
#REMIX               y
#REPGP               y

## In middleman mode, mail is randhopped through this chain
#FORWARDTO           *

## Filter binaries and replace them with "[...]"
## Note: destroys even PGP messages sometimes
BINFILTER           n

## Allow users to add their address to the dest.blk file by sending the
## remailer a message containing the line "destination-block"
## Note: as no challenge-response mechanisms are used (yet),
##       attackers could dest-block arbitrary addresses
AUTOBLOCK           y

## Automatically respond to non-remailer mail and mail to COMPLAINTS address
AUTOREPLY           n

## List statistics on intermediate vs. final delivery in remailer-stats.
STATSDETAILS        y

## List known remailers and their keys in remailer-conf reply
LISTSUPPORTED       y

## Maximum chain length for message forwarding requested by
## Rand-Hop and Remix-To directives
MAXRANDHOPS         5

## Maximum size for Inflate: padding in kB.  0 means padding is not allowed
INFLATEMAX          50

## Limits the number of allowed recipients in outgoing mail
## Anything that exceeds this number is dropped silently
MAXRECIPIENTS       5

## Passphrase to protect secret keys
#PASSPHRASE          raboof

## Maximum message size in kB (0 for no limit):
SIZELIMIT           0

## Remailing strategy:
MAILINTIME          5m
SENDPOOLTIME        15m
POOLSIZE            45
RATE                65

## Dummy generation probabilities
INDUMMYP            10
OUTDUMMYP           90 

## How long to store packet IDs and incomplete message parts
IDEXP               7d
PACKETEXP           7d

## Client settings for Rand-Hop: directives and dummy messages
CHAIN               *,*,*,*
DISTANCE            2
MINREL              98
RELFINAL            99
MAXLAT              36h
MINLAT              5m

## This file lists remailers which should not be used in randomly generated
## remailer chains
STAREX              starex.txt

## Path to inews, or address of mail-to-news gateway
## Leave empty to disable mix-post capability flag
## Add more mail2news gateways to increase posting reliability
## (and mail load on your MTA). Additional m2n include:
## mail2news@news.gradwell.net
#NEWS                mail2news@dizum.com,mail2news@anon.lcs.mit.edu
ORGANIZATION        Anonymous Posting Service

## Anti-spam message IDs on Usenet (MD5 of message body)?
MID                 y

## Precedence: header to set on remailed messages
#PRECEDENCE          anon

## Enable either SENDMAIL/SENDANONMAIL (pipe into sendmail program)
## or SMTPRELAY (SMTP delivery over TCP)
SENDMAIL            /usr/lib/sendmail -t
#SENDANONMAIL        sendmessage.sh

#SMTPRELAY           smtp.example.net
#SMTPUSERNAME        foo
#SMTPPASSWORD        bar
#HELONAME            example.net
#ENVFROM             mix-bounce@example.net

## Where to log error messages:
ERRLOG              error.log
VERBOSE             2

## Where to read mail messages from
## trailing "/" indicates maildir-style folder
## leave empty when you feed mixmaster through stdin (e.g. from procmail)
#MAILIN              /var/mail/mixmaster

## POP3 configuration
POP3CONF            pop3.cfg
POP3TIME            1h
POP3SIZELIMIT       0
POP3DEL             y

## Where to store non-remailed messages
## prefix with "|" to pipe into program
## treated as email address if it contains an "@"
MAILBOX             mbox
#MAILABUSE           mbox.abuse
#MAILBLOCK           mbox.block
#MAILUSAGE           /dev/null
#MAILANON            /dev/null
#MAILERROR           /dev/null
#MAILBOUNCE          mbox.bounce

## Where to find variable remailer keyrings and statistics
PGPREMPUBASC        pubring.asc
PUBRING             pubring.mix
TYPE1LIST           rlist.txt
TYPE2REL            mlist.txt
TYPE2LIST           type2.list

## If you run your own pinger, make stats/ a symlink to your results directory
## and enable these instead
#PGPREMPUBASC        stats/pgp-all.asc
#PUBRING             stats/pubring.mix
#TYPE1LIST           stats/rlist.txt
#TYPE2REL            stats/mlist.txt
#TYPE2LIST           stats/type2.list

## Where to find various textfiles
DISCLAIMFILE        disclaim.txt
FROMDSCLFILE        fromdscl.txt
MSGFOOTERFILE       footer.txt
HELPFILE            help.txt
ADMKEY-FILE         adminkey.txt
ABUSEFILE           abuse.txt
REPLYFILE           reply.txt
USAGEFILE           usage.txt
BLOCKFILE           blocked.txt

## List of blocked source addresses
SOURCE-BLOCK        source.blk

## List of unwanted header fields
HDRFILTER           header.blk

## List of blocked destination addresses
DESTBLOCK           dest.blk rab.blk

## List of addresses to which Mixmaster will deliver, even in middleman mode
DESTALLOW           dest.alw

## Pid file in daemon mode
PIDFILE             mixmaster.pid
