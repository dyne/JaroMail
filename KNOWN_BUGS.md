# Known bugs for Jaro Mail

## 1.3.1

locking release time problems occurring often on `jaro send`

```
[!] forced removal of lock left by pid 8851: msmtp.mail.dyne.org.12Mar14.22032
/dev/shm/tmp.jaromail.jrml/msmtp.mail.dyne.org.12Mar14.22032: fatal: could not lstat: No such file or directory
```

When occurring, first mail in queue is sent, next one not.

