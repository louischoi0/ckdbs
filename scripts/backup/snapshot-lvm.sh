#!/bin/sh
# Path D's snapshot on LVM (BR-S3): the reference procedure. Not run by the
# drills here - the development VM grants no root (H9) - and the engine
# carries no vendor logic: this is the operator's side of the contract.
#
# The contract: an atomic snapshot of the volume holding `data_file`, taken
# between BACKUP START and BACKUP STOP. `lvcreate --snapshot` suspends the
# device-mapper target, which freezes the filesystem first, so every write
# the kernel accepted is in the snapshot whole. The log is not taken from
# the snapshot: BACKUP STOP copies the window's segments into the backup
# after it, which is what makes a separate WAL volume safe without ordering
# two snapshots.
#
#   tools/kds_backup.py snapshot --target DIR \
#       --command 'scripts/backup/snapshot-lvm.sh vg0 data /srv/kds/kds.db'
set -eu
vg="$1"
lv="$2"
data_file="$3"
dir="${KDS_BACKUP_DIR:?KDS_BACKUP_DIR is not set}"
snap="kds-backup-${KDS_BACKUP_ID:?}"
mnt="$(mktemp -d)"
lvcreate --snapshot --size 10G --name "$snap" "/dev/$vg/$lv"
trap 'umount "$mnt" 2>/dev/null; lvremove -f "/dev/$vg/$snap"; rmdir "$mnt"' EXIT
mount -o ro "/dev/$vg/$snap" "$mnt"
# The data file's path inside the mounted snapshot: its path relative to the
# volume's mount point.
rel="$(realpath --relative-to="$(findmnt -no TARGET --target "$data_file")" "$data_file")"
mkdir -p "$dir"
cp "$mnt/$rel" "$dir/kds.db.tmp"
sync "$dir/kds.db.tmp"
mv "$dir/kds.db.tmp" "$dir/kds.db"
sync "$dir"
