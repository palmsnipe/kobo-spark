#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
app=/mnt/onboard/.adds/kobo-spark
lock=/tmp/kobo-spark.lock
# The launcher and its guard can both arrive here; only one performs recovery.
mkdir "$lock/recovering" 2>/dev/null || exit 0
trap 'rmdir "$lock/recovering" 2>/dev/null || true' EXIT
retry_later() {
    # Wake the guard on its next tick rather than waiting another 45 seconds.
    rm -f "$lock/heartbeat"
    if ! pidof nickel >/dev/null; then
        killall -q -TERM sickel-launcher sickel strickel hindenburg 2>/dev/null || true
    fi
    exit 1
}
if [ -f "$lock/child" ]; then
    child=$(cat "$lock/child")
    comm=$(cat /proc/$child/comm 2>/dev/null || true)
    if [ "$comm" = kobo-spark ]; then
        kill -TERM "$child" 2>/dev/null || true
        tries=0
        while [ "$(cat /proc/$child/comm 2>/dev/null || true)" = "$comm" ] && [ "$tries" -lt 5 ]; do
            sleep 1; tries=$((tries + 1))
        done
        [ "$(cat /proc/$child/comm 2>/dev/null || true)" != "$comm" ] || kill -KILL "$child" 2>/dev/null || true
        tries=0
        while [ "$(cat /proc/$child/comm 2>/dev/null || true)" = "$comm" ] && [ "$tries" -lt 10 ]; do
            sleep 0.2; tries=$((tries + 1))
        done
        # A task stuck in the kernel must not overlap a second framebuffer UI.
        if [ "$(cat /proc/$child/comm 2>/dev/null || true)" = "$comm" ]; then
            echo "App has not released its devices; retrying reader recovery later"; retry_later
        fi
    fi
fi
if [ -f "$lock/reader-env" ]; then
    while IFS= read -r entry; do
        case "$entry" in
            DBUS_SESSION_BUS_ADDRESS=*|NICKEL_HOME=*|WIFI_MODULE=*|LANG=*|INTERFACE=*|BATTERY=*|LD_LIBRARY_PATH=*|QT_GSTREAMER_*=*|HOME=*|PLATFORM=*|PRODUCT=*|PATH=*|UBOOT_MMC=*|UBOOT_RECOVERY=*) export "$entry";;
        esac
    done < "$lock/reader-env"
    cd /
    # Match rcS: the FIFO and hardware service must exist before Nickel starts.
    # Without the FIFO, udev events never reach the reader after our takeover.
    if ! pidof nickel >/dev/null; then
        rm -f /tmp/nickel-hardware-status
        mkfifo /tmp/nickel-hardware-status
        if ! pidof hindenburg >/dev/null; then
            nohup /usr/local/Kobo/hindenburg >>"$app/reader.log" 2>&1 </dev/null &
        fi
        LIBC_FATAL_STDERR_=1 nohup /usr/local/Kobo/nickel -platform kobo -skipFontLoad >>"$app/reader.log" 2>&1 </dev/null &
        # Replay hardware state as rcS does, including touch/display and Wi-Fi.
        nohup udevadm trigger >>"$app/reader.log" 2>&1 </dev/null &
    fi
    # Nickel starts its own font-loader and sickel watchdog. Keep our lease and
    # recovery guard while it settles; a PID alone does not mean a usable UI.
    tries=0
    while ! pidof nickel >/dev/null && [ "$tries" -lt 20 ]; do sleep 0.2; tries=$((tries + 1)); done
    if ! pidof nickel >/dev/null; then echo "Reader failed to start; leaving guard active"; retry_later; fi
    sleep 5
    if ! pidof nickel >/dev/null; then echo "Reader exited during startup; leaving guard active"; retry_later; fi
    if ! pidof hindenburg >/dev/null; then
        nohup /usr/local/Kobo/hindenburg >>"$app/reader.log" 2>&1 </dev/null &
    fi
fi
echo "Reader handoff complete"
echo kobo-spark > /sys/power/wake_unlock 2>/dev/null || true
rm -f "$lock/child" "$lock/reader-env" "$lock/active" "$lock/heartbeat"
rmdir "$lock/recovering" "$lock" 2>/dev/null || true
