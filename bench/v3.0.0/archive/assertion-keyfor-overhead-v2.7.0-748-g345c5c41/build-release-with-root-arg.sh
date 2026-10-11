#!/bin/sh
# Release build into build-release/, refreshing its binaries in place.
# Paths resolve from this script's own location, so it builds whichever
# checkout or worktree it sits in. Arguments are passed through as build
# targets: `scripts/build-release.sh kds_server` builds only the server.
set -e
ROOT="$1"; shift
BUILD="$ROOT/build-release"

# A cached OpenSSL path under a gone session scratchpad is reused by
# find_package without checking it exists; clear it so the system one is found.
UNSET=""
INC="$(sed -n 's/^OPENSSL_INCLUDE_DIR:PATH=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
if [ -n "$INC" ] && [ ! -d "$INC" ]; then
    UNSET="-U OPENSSL_* -U FIND_PACKAGE_MESSAGE_DETAILS_OpenSSL"
fi

# UNSET is word-split on purpose; -f keeps OPENSSL_* from globbing the cwd.
set -f
# shellcheck disable=SC2086
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release $UNSET
if [ $# -gt 0 ]; then
    cmake --build "$BUILD" -j"$(nproc)" --target "$@"
else
    cmake --build "$BUILD" -j"$(nproc)"
fi
