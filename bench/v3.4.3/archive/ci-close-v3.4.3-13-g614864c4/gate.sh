#!/bin/bash
# wait until no compiler/test process has been seen for 20 s
q=0
while [ $q -lt 10 ]; do
  s=$( (pgrep -x cc1plus; pgrep -x clang; pgrep -x 'clang++'; pgrep -x kds_tests) | wc -l)
  if [ "$s" -eq 0 ]; then q=$((q+1)); else q=0; fi
  sleep 2
done
