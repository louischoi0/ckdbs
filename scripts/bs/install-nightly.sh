#!/bin/bash
# Installs BS's nightly runner on this host (BS-Q1 (b): a cron job on the
# benchmark host, outside measurement windows). Idempotent: a second run
# re-points it.
#
#   $BS_NIGHTLY_HOME/src      a clone of $BS_NIGHTLY_URL, moved to $BS_NIGHTLY_REF
#                             each night
#   $BS_NIGHTLY_HOME/run.sh   cron's entry: one run at a time, fetch, then
#                             src/scripts/bs/nightly.sh - so the runner a night
#                             executes is the one on the ref that night
#   $BS_NIGHTLY_HOME/reports  the reports nightly.sh writes; latest.md first
#
# Defaults: origin's URL and `main`. Before BS lands on main the runner can
# follow the work branch in the local repository, which publishes nothing:
#   BS_NIGHTLY_URL=/home/cdkbs/ckdbs BS_NIGHTLY_REF=bs-test-hardening install-nightly.sh
#
# The slot is 19:30 UTC (04:30 KST): main's commit times since 2026-10-06
# are thinnest between 17:00 and 23:00 UTC. A night that finds a measurement
# running skips itself and says so (nightly.sh).
set -euo pipefail
H=${BS_NIGHTLY_HOME:-/home/cdkbs/kds-nightly}
URL=${BS_NIGHTLY_URL:-$(git -C "$(dirname "$0")" remote get-url origin)}
REF=${BS_NIGHTLY_REF:-main}
SLOT=${BS_NIGHTLY_SLOT:-30 19 * * *}
MARK='# kds-bs-nightly (scripts/bs/install-nightly.sh)'

mkdir -p "$H/reports"
[ -d "$H/src/.git" ] || git clone -q "$URL" "$H/src"
git -C "$H/src" remote set-url origin "$URL"

cat > "$H/run.sh" <<EOF
#!/bin/bash
# Written by scripts/bs/install-nightly.sh; cron's entry for BS's nightly,
# following $REF of $URL.
set -u
exec 9>"$H/lock"
flock -n 9 || { echo "\$(date -u +%FT%TZ) a run is still going; this one skipped"; exit 0; }
cd "$H/src" || exit 2
if ! git fetch -q origin "$REF" || ! git checkout -q --detach FETCH_HEAD; then
    echo "- \$(date -u +%F) fetch of $REF failed; no run" >> "$H/reports/index.md"
    exit 2
fi
BS_REF="$REF" BS_REPORTS="$H/reports" exec "$H/src/scripts/bs/nightly.sh"
EOF
chmod +x "$H/run.sh"

line="$SLOT PATH=/usr/local/bin:/usr/bin:/bin $H/run.sh >> $H/cron.log 2>&1 $MARK"
# Only "no crontab" reads as empty: any other failure of `crontab -l` would
# install this line alone over the user's other entries.
if ! current=$(crontab -l 2>"$H/crontab.err"); then
    grep -q '^no crontab for ' "$H/crontab.err" ||
        { cat "$H/crontab.err" >&2; echo "install-nightly: crontab -l failed; crontab left as it was" >&2; exit 1; }
    current=""
fi
printf '%s\n' "$current" > "$H/crontab.before"
{ printf '%s\n' "$current" | grep -vF "$MARK" || true; echo "$line"; } | crontab -
crontab -l | grep -F "$MARK"
echo "install-nightly: $H follows $REF of $URL; reports in $H/reports"
