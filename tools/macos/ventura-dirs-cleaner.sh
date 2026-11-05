#!/bin/sh
# Starbite (2026-11-05): /usr/libexec/dirs_cleaner, the boot task that empties /tmp and /var/run, keys on the
# boot UUID, which launchd cannot get on our root ("Unable to get bootuuid"). After a reset the previous
# boot's /var/run stayed: com.apple.loginwindow.didRunThisBoot made launchd never start loginwindow (black
# screen with a pointer on every second boot). Empty /var/run (files, sockets; not folders) first.
/usr/bin/find /private/var/run -mindepth 1 -maxdepth 1 ! -type d -exec /bin/rm -f {} + 2>/dev/null
exec /usr/libexec/dirs_cleaner.starbite-orig "$@"
