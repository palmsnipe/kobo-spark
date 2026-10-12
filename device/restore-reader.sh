#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
app=/mnt/onboard/.adds/kobo-spark
lock=/tmp/kobo-spark.lock
# The launcher and its guard can both arrive here; only one performs recovery.
mkdir "$lock/recovering" 2>/dev/null || exit 0
if [ -f "$lock/child" ]; then
    child=$(cat "$lock/child")
    comm=$(cat /proc/$child/comm 2>/dev/null || true)
    if [ "$comm" = kobo-spark ]; then
        kill -TERM "$child" 2>/dev/null || true
        tries=0
        while [ "$(cat /proc/$child/comm 2>/dev/null)" = "$comm" ] && [ "$tries" -lt 5 ]; do
            sleep 1; tries=$((tries + 1))
        done
        [ "$(cat /proc/$child/comm 2>/dev/null)" != "$comm" ] || kill -KILL "$child" 2>/dev/null || true
    fi
fi
if [ -f "$lock/reader-env" ]; then
    while IFS= read -r entry; do
        case "$entry" in
            DBUS_SESSION_BUS_ADDRESS=*|NICKEL_HOME=*|WIFI_MODULE=*|LANG=*|INTERFACE=*|BATTERY=*|LD_LIBRARY_PATH=*|QT_GSTREAMER_*=*|HOME=*) export "$entry";;
        esac
    done < "$lock/reader-env"
    cd /
    pidof hindenburg >/dev/null || nohup /usr/local/Kobo/hindenburg >>"$app/reader.log" 2>&1 </dev/null &
    # Nickel starts its own font-loader and watchdog helpers.
    if ! pidof nickel >/dev/null; then
        LIBC_FATAL_STDERR_=1 nohup /usr/local/Kobo/nickel -platform kobo -skipFontLoad >>"$app/reader.log" 2>&1 </dev/null &
    fi
fi
echo kobo-spark > /sys/power/wake_unlock 2>/dev/null || true
rm -f "$lock/child" "$lock/reader-env" "$lock/active" "$lock/heartbeat"
rmdir "$lock/recovering" "$lock" 2>/dev/null || true
