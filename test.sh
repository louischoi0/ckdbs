#!/bin/sh
set -e
./build.sh
ctest --test-dir build --output-on-failure
