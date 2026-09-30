#!/bin/bash
cd /home/cdkbs/bench-runs/rebaseline-9a0525d
P=cd2- CONF3="cabin_optimizer_cooldown_half_lives = 2" bash run_s4b.sh
P=r2- bash run_s3.sh
P=r3- bash run_s3.sh
touch chain2.done
