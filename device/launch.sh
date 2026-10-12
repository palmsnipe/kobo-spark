#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
app=/mnt/onboard/.adds/kobo-spark
lock=/tmp/kobo-spark.lock
mkdir "$lock" 2>/dev/null || exit 1
chmod 700 "$lock"
child=""
guard=""
cleanup() {
    trap - EXIT HUP INT TERM
    [ -z "$guard" ] || kill "$guard" 2>/dev/null || true
    "$app/restore-reader.sh"
    [ -z "$child" ] || wait "$child" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 143' HUP INT TERM
exec >>"$app/app.log" 2>&1
echo "Launching Kobo Spark"
sleep 1
reader=$(pidof nickel)
[ -n "$reader" ] && [ "${reader#* }" = "$reader" ]
tr '\000' '\n' < "/proc/$reader/environ" > "$lock/reader-env"
chmod 600 "$lock/reader-env"
touch "$lock/active" "$lock/heartbeat"
# Recover even if rendering hangs or the launcher itself is killed.
(
    while [ -e "$lock/active" ]; do
        sleep 5
        stamp=$(stat -c %Y "$lock/heartbeat" 2>/dev/null || echo 0)
        now=$(date +%s)
        if [ "$((now-stamp))" -gt 45 ]; then "$app/restore-reader.sh"; break; fi
    done
) &
guard=$!
sync
# Stopping Nickel releases its exclusive touch/page/power input grabs.
killall -q -TERM sickel strickel fontickel hindenburg nickel adobehost foxitpdf iink 2>/dev/null || true
tries=0
while pidof nickel >/dev/null && [ "$tries" -lt 20 ]; do sleep 0.2; tries=$((tries + 1)); done
if pidof nickel >/dev/null; then echo "Reader did not stop; returning to it"; exit 1; fi
rm -f /tmp/nickel-hardware-status
"$app/kobo-spark" "$@" &
child=$!
echo "$child" > "$lock/child"
result=0
wait "$child" || result=$?
child=""
rm -f "$lock/child"
echo "Kobo Spark exited with status $result"
exit "$result"
