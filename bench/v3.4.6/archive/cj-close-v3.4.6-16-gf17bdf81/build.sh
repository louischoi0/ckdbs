#!/bin/bash
# usage: build.sh label srcdir [extra cxx flags]
S=/tmp/claude-1000/-home-louis-ckdbs/c0ca10d8-b854-48ed-840b-6fe2f0acb530/scratchpad
L=$1; SRC=$2; FL=$3
cmake -S $SRC -B $S/build-$L -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="$FL" >$S/cfg-$L.log 2>&1 && cmake --build $S/build-$L --target kds_server -j3 >$S/bld-$L.log 2>&1
echo $? >$S/done-$L
