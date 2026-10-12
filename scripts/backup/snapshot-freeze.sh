#!/bin/sh
# Path D's snapshot by freezing the server (BR-S3, H9): every thread of the
# process stops (SIGSTOP), the data file is copied, the process resumes
# (SIGCONT). A write the kernel accepted is in the copy whole and none is half
# in it - the property an atomic volume snapshot gives - because no thread of
# the server can be inside a pwrite while it is stopped.
#
# For a host with no snapshot-capable volume, and for drills. The process is
# frozen for the copy's length: connections stall, none is dropped.
#
# Run by `kds_backup snapshot --command`, which sets KDS_BACKUP_DIR:
#   tools/kds_backup.py snapshot --target DIR \
#       --command 'scripts/backup/snapshot-freeze.sh <server-pid> <data_file>'
set -eu
pid="$1"
data_file="$2"
dir="${KDS_BACKUP_DIR:?KDS_BACKUP_DIR is not set}"
mkdir -p "$dir"
kill -STOP "$pid"
# Resumed whatever the copy did: a server left stopped is an outage.
trap 'kill -CONT "$pid"' EXIT
# `kill` returns before the threads have stopped: wait until every one of
# them reports state T, so no pwrite is still in flight when cp reads.
while :; do
    running=0
    for stat in /proc/"$pid"/task/*/stat; do
        state=$(sed 's/^.*) //' "$stat" | cut -d' ' -f1)
        [ "$state" = "T" ] || [ "$state" = "t" ] || running=1
    done
    [ "$running" -eq 0 ] && break
    sleep 0.01
done
cp "$data_file" "$dir/kds.db.tmp"
kill -CONT "$pid"
trap - EXIT
sync "$dir/kds.db.tmp"
mv "$dir/kds.db.tmp" "$dir/kds.db"
sync "$dir"
