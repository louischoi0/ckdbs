#!/bin/sh
# DI-S0: the four server binaries of the premise measurement, built Release from one
# clean export of the commit DI opened from (1c7bb774), one directory, in sequence.
#   A   the commit as it is
#   A0  A with RecordStatement's call of StatementStats::Record compiled out
#   B1  A + the binding spike (b1.patch + statement_binding.hpp): the key
#       (fetch_id, kind, namespace, up to 16 distinct oids)
#   B2  B1 with the binding cut to its first relation
# Never committed; each binary is hashed into $R/bin/sha256.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
R=/home/louis/bench-runs/di-s0
SRC=$R/src
mkdir -p "$R/bin"
rm -rf "$SRC"
mkdir -p "$SRC"
tar -x -C "$SRC" -f "$HERE/head.tar"
build() {
    cmake -S "$SRC" -B "$SRC/build-release" -DCMAKE_BUILD_TYPE=Release \
        -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > "$R/build-$1.log" 2>&1
    cmake --build "$SRC/build-release" -j"$(nproc)" --target kds_server >> "$R/build-$1.log" 2>&1
    cp "$SRC/build-release/kds_server" "$R/bin/kds_$1"
    echo "built $1 $(date -u +%FT%TZ)"
}
build A
python3 "$HERE/a0.py" "$SRC" apply
build A0
python3 "$HERE/a0.py" "$SRC" revert
(cd "$SRC" && patch -p1 -s < "$HERE/b1.patch")
cp "$HERE/statement_binding.hpp" "$SRC/include/kds/stats/statement_binding.hpp"
build B1
python3 "$HERE/b2.py" "$SRC"
build B2
(cd "$R/bin" && sha256sum kds_A kds_A0 kds_B1 kds_B2 > sha256)
cat "$R/bin/sha256"
echo "ARMS DONE $(date -u +%FT%TZ)"
