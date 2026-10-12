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
    # Keep the independent guard alive until the reader handoff is complete.
    "$app/restore-reader.sh" || echo "Reader recovery failed; guard will retry"
    if [ ! -e "$lock/active" ]; then
        [ -z "$guard" ] || kill "$guard" 2>/dev/null || true
    fi
    [ -z "$child" ] || wait "$child" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 143' HUP INT TERM
exec >>"$app/app.log" 2>&1
echo "Launching Kobo Spark"
sleep 1
# NickelMenu asks the stock Wi-Fi manager to connect before spawning us. Leave
# Nickel alive while it finishes associating, instead of interrupting it early.
tries=0
while [ "$(cat /sys/class/net/${INTERFACE:-wlan0}/carrier 2>/dev/null || true)" != 1 ] && [ "$tries" -lt 10 ]; do
    sleep 1; tries=$((tries + 1))
done
reader=$(pidof nickel)
[ -n "$reader" ] && [ "${reader#* }" = "$reader" ]
tr '\000' '\n' < "/proc/$reader/environ" > "$lock/reader-env"
chmod 600 "$lock/reader-env"
touch "$lock/heartbeat"
# Recover even if rendering hangs or the launcher itself is killed.
(
    while [ -d "$lock" ]; do
        sleep 5
        stamp=$(stat -c %Y "$lock/heartbeat" 2>/dev/null || echo 0)
        now=$(date +%s)
        if [ "$((now-stamp))" -gt 45 ]; then
            if "$app/restore-reader.sh" && [ ! -e "$lock/active" ]; then break; fi
        fi
    done
) &
guard=$!
# Disable watchdogs completely before taking down their reader. Sending all
# signals together could leave a live watchdog observing a missing Nickel.
killall -q -TERM sickel-launcher hindenburg sickel strickel 2>/dev/null || true
tries=0
while { pidof sickel >/dev/null || pidof hindenburg >/dev/null || pidof strickel >/dev/null; } && [ "$tries" -lt 20 ]; do
    sleep 0.2; tries=$((tries + 1))
done
killall -q -KILL sickel-launcher hindenburg sickel strickel 2>/dev/null || true
tries=0
while { pidof sickel >/dev/null || pidof hindenburg >/dev/null || pidof strickel >/dev/null; } && [ "$tries" -lt 10 ]; do
    sleep 0.2; tries=$((tries + 1))
done
if pidof sickel >/dev/null || pidof hindenburg >/dev/null || pidof strickel >/dev/null; then
    echo "Watchdog did not stop; keeping the reader running"; exit 1
fi
touch "$lock/active"
echo 'kobo-spark 90000000000' > /sys/power/wake_lock 2>/dev/null || true
# Stopping Nickel releases its exclusive touch/page/power input grabs.
killall -q -TERM nickel fontickel adobehost foxitpdf iink 2>/dev/null || true
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
