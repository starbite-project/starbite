#!/bin/sh
# Starbite diagnostics for macOS on the phone, run at login by /Library/LaunchAgents/org.starbite.diag.plist.
# It does nothing unless the kernel command line has sbdiag=1, so it can stay on the card: a test payload turns it on.
# Everything goes to the console, which the kernel tees into its log ("UCON SBDIAG ..." / "UCON SBLOG ...").
# v2 (S6p 2026-11-16): the apps' files and LaunchServices records, a rescan of the application folders, the audio
# daemon, and System Settings left open at the end (for a photo of its icons).
case " $(/usr/sbin/sysctl -n kern.bootargs) " in *" sbdiag=1 "*) ;; *) exit 0 ;; esac
exec >/dev/console 2>&1
echo "SBDIAG start v2"
sleep 100
# df's "Available" leaves out HFS's 1 % reserve (164 MiB of the 16 GiB root): free = Available + 164
/bin/df -m / | /usr/bin/sed 's/^/SBDIAG df: /'
/usr/bin/du -sm /.Spotlight-V100 /private/var/db/Spotlight-V100 /private/var/db/diagnostics /private/var/db/uuidtext /private/var/log /private/var/folders /Library/Logs /Users 2>/dev/null | /usr/bin/tr '\n' ' ' | /usr/bin/sed 's/^/SBDIAG du (MiB): /'
echo
LSR=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
APPS="Books Freeform Maps Music Photos TV Weather"
# S6j: "Unable to find application named" for six of the seven. Are the executables there, and does LaunchServices
# know the bundles (it met them first without an executable)?
for a in $APPS; do
	echo "SBDIAG file $a: $(/bin/ls -l "/System/Applications/$a.app/Contents/MacOS/" 2>&1 | /usr/bin/tail -n +2 | /usr/bin/tr '\n' ' ')"
done
$LSR -dump 2>/dev/null > /tmp/sbdiag-ls.txt
echo "SBDIAG lsregister dump: $(/usr/bin/wc -l < /tmp/sbdiag-ls.txt) lines"
for a in $APPS; do
	echo "SBDIAG ls record $a before: $(/usr/bin/grep -c "/System/Applications/$a.app\$" /tmp/sbdiag-ls.txt)"
done
# the rescan (what a one-time fix would run)
$LSR -f -R /System/Applications /Applications 2>&1 | /usr/bin/tail -3 | /usr/bin/sed 's/^/SBDIAG lsregister: /'
echo "SBDIAG lsregister rc $?"
sleep 5
for a in $APPS; do
	/usr/bin/open -a "$a" 2>&1 | /usr/bin/sed "s/^/SBDIAG open $a: /"
	sleep 14
	if /usr/bin/pgrep -x "$a" >/dev/null; then echo "SBDIAG app $a: running"; else echo "SBDIAG app $a: NOT running"; fi
	/usr/bin/pkill -x "$a"
	sleep 2
done
# audio daemon (S6p puts it back) and Spotlight
echo "SBDIAG coreaudiod: $(/usr/bin/pgrep -l coreaudiod | /usr/bin/tr '\n' ' ')"
echo "SBDIAG mds: $(/usr/bin/pgrep -l -f 'Support/mds' | /usr/bin/tr '\n' ' ')"
/usr/bin/mdutil -s / 2>&1 | /usr/bin/sed 's/^/SBDIAG mdutil: /'
/usr/bin/mdfind -name Desktop 2>&1 | /usr/bin/head -5 | /usr/bin/sed 's/^/SBDIAG mdfind: /'
# the programs' own error lines (S3zy: log show works on the phone)
show() {
	/usr/bin/log show --last 9m --style compact --predicate "$2" 2>&1 | /usr/bin/head -"$3" | /usr/bin/cut -c1-330 | /usr/bin/sed "s/^/SBLOG $1 /"
	echo "SBLOG $1 done"
}
show nc '(process == "NotificationCenter" OR process == "usernotificationsd" OR process == "containermanagerd") AND (messageType == error OR messageType == fault)' 50
show wall '(process == "WallpaperAgent" OR process == "WallpaperVideoExtension" OR process == "WallpaperImageExtension") AND (messageType == error OR messageType == fault)' 40
show icon '(process == "iconservicesagent" OR process == "iconservicesd" OR process == "System Settings") AND (messageType == error OR messageType == fault)' 60
show audio '(process == "coreaudiod") AND (messageType == error OR messageType == fault)' 30
show apps '(process == "Books" OR process == "Freeform" OR process == "Maps" OR process == "Music" OR process == "Photos" OR process == "TV" OR process == "Weather" OR process == "launchservicesd" OR process == "lsd" OR process == "open") AND (messageType == error OR messageType == fault)' 60
# services that started and exited at once every 10 s in S6i/S6j: their last words
show exits '(process == "usernotificationsd" OR process == "nsurlsessiond" OR process == "ionodecache" OR process == "wifip2pd" OR process == "promotedcontentd" OR process == "maild" OR process == "appstoreagent" OR process == "networkserviceproxy") AND (messageType == error OR messageType == fault)' 60
# what launchd itself logs: exits with their reasons, without the lookup noise
/usr/bin/grep -E "exited due to|Service only ran for|failed lookup" /private/var/log/com.apple.xpc.launchd/launchd.log 2>/dev/null | /usr/bin/sed -E 's/^[0-9-]+ [0-9:.]+ //; s/\[[0-9]+\]//g; s/ran for [0-9]+ms/ran/' | /usr/bin/sort | /usr/bin/uniq -c | /usr/bin/sort -rn | /usr/bin/head -45 | /usr/bin/cut -c1-230 | /usr/bin/sed 's/^/SBLOG launchd /'
# left open for a photo of the icons
/usr/bin/open -a "System Settings"
echo "SBDIAG done"
