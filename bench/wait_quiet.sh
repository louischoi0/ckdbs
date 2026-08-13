#!/usr/bin/env bash
# Blocks until no compiler and no package job is running and the 1-minute load
# has come back down. A measurement taken beside a build is not a measurement:
# a concurrent build has already cost this suite one whole run.
set -uo pipefail
while true; do
    busy=0
    # -x throughout: an unanchored `pgrep ld` regex-matches every
    # `systemd-journald` on the box and the gate never opens.
    pgrep -x cc1plus   > /dev/null 2>&1 && busy=1
    pgrep -x ld        > /dev/null 2>&1 && busy=1
    pgrep -x kds_tests > /dev/null 2>&1 && busy=1
    # `unattended-upgrade` proper, never `unattended-upgrade-shutdown
    # --wait-for-signal`: that one is resident for the life of the boot and
    # burns no CPU, and matching it blocks this gate forever. That bug cost
    # this run half an hour of a gate that could never open.
    pgrep -f "unattended-upgrade --" > /dev/null 2>&1 && busy=1
    pgrep -x apt-get > /dev/null 2>&1 && busy=1
    pgrep -x dpkg    > /dev/null 2>&1 && busy=1
    if [ "$busy" -eq 0 ]; then
        load=$(cut -d' ' -f1 /proc/loadavg)
        if awk -v l="$load" 'BEGIN{exit !(l < 0.70)}'; then
            echo "quiet: loadavg $load"
            exit 0
        fi
    fi
    sleep 15
done
