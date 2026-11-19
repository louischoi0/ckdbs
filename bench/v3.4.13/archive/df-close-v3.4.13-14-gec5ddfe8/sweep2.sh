#!/bin/bash
cd /home/louis/dfclose/run
./life.sh r200 3 6 1 3 12000 24 select-pk,select-index,update-lit 200 32300 5
./life.sh r10k 3 6 1 3 12000 24 select-pk,select-index,update-lit 10000 32400 5
echo "$(date -u +%FT%TZ) sweep2 ALLDONE" >> status.txt
