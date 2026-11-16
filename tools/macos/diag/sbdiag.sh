#!/bin/sh
# Starbite diagnostics for macOS on the phone, run at login by /Library/LaunchAgents/org.starbite.diag.plist.
# It does nothing unless the kernel command line has sbdiag=1, so it can stay on the card: a test payload turns it on.
# Everything goes to the console, which the kernel tees into its log ("UCON SBDIAG ..." / "UCON SBLOG ...").
case " $(/usr/sbin/sysctl -n kern.bootargs) " in *" sbdiag=1 "*) ;; *) exit 0 ;; esac
exec >/dev/console 2>&1
echo "SBDIAG start"
sleep 100
/bin/df -m / | /usr/bin/sed 's/^/SBDIAG df: /'
# do the apps whose executables the root build had left out start now?
for a in Books Freeform Maps Music Photos TV Weather; do
	/usr/bin/open -a "$a"
	sleep 14
	if /usr/bin/pgrep -x "$a" >/dev/null; then echo "SBDIAG app $a: running"; else echo "SBDIAG app $a: NOT running"; fi
	/usr/bin/pkill -x "$a"
	sleep 2
done
# Spotlight
echo "SBDIAG mds: $(/usr/bin/pgrep -l -f 'Support/mds' | /usr/bin/tr '\n' ' ')"
/usr/bin/mdutil -s / 2>&1 | /usr/bin/sed 's/^/SBDIAG mdutil: /'
/usr/bin/mdfind -name Desktop 2>&1 | /usr/bin/head -5 | /usr/bin/sed 's/^/SBDIAG mdfind: /'
# the programs' own error lines (S3zy: log show works on the phone)
show() {
	/usr/bin/log show --last 7m --style compact --predicate "$2" 2>&1 | /usr/bin/head -"$3" | /usr/bin/sed "s/^/SBLOG $1 /"
	echo "SBLOG $1 done"
}
show nc '(process == "NotificationCenter" OR process == "chronod" OR process == "containermanagerd") AND (messageType == error OR messageType == fault OR eventMessage CONTAINS[c] "group")' 70
show wall '(process == "WallpaperAgent" OR process == "WallpaperVideoExtension" OR process == "WallpaperImageExtension") AND (messageType == error OR messageType == fault OR eventMessage CONTAINS[c] "image" OR eventMessage CONTAINS[c] "choice")' 70
show icon '(process == "iconservicesagent" OR process == "iconservicesd" OR process == "System Settings" OR subsystem CONTAINS[c] "iconservices") AND (messageType == error OR messageType == fault OR eventMessage CONTAINS[c] "icon")' 80
show mds '(process == "mds" OR process == "mds_stores" OR process == "Finder") AND (messageType == error OR messageType == fault)' 50
show apps '(process == "Books" OR process == "Freeform" OR process == "Maps" OR process == "Music" OR process == "Photos" OR process == "TV" OR process == "Weather" OR process == "runningboardd" OR process == "launchservicesd") AND (messageType == error OR messageType == fault)' 80
echo "SBDIAG done"
