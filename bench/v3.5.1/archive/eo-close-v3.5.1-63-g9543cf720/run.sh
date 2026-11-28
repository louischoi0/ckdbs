#!/bin/bash
# EN-S4: the timed pass (3 rounds, KDS, PostgreSQL, Oracle), then
# PostgreSQL's profile pass (1 round), each under the 60 s watchdog.
R=/home/ukjin/bench-runs/en-v3.5.1-63-g9543cf720
S=<private: the Oracle control's client libraries and secret, not committed>
COMMON=(--work "$R/work" --kds-bin "$R/kds_server" --ora-home "$S/oralib" --ora-secret "$S/ora.env")
date -u +%FT%TZ > "$R/start-time.txt"
"$R/tools/watchdog.sh" "$R/out" 15 -- --out "$R/out" "${COMMON[@]}" --rounds 3 --pass timed
"$R/tools/watchdog.sh" "$R/out" 15 -- --out "$R/out" "${COMMON[@]}" --rounds 1 --pass profile
date -u +%FT%TZ > "$R/end-time.txt"
