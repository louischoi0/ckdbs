#!/bin/bash
# The measured binary, pinned to the P-cores (CPUs 0-7). exec keeps the pid.
exec taskset -c 0-7 /home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354/bin/kds_server "$@"
