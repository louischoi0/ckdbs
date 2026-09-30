#!/bin/bash
cd /home/cdkbs/bench-runs/rebaseline-9a0525d
P=r2- bash run_s02.sh
P=r3- bash run_s02.sh
touch reps.done
