# sourced by every cell script
export WT=/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue
export RUN=/home/cdkbs/bench-runs/bp-s1
export BIN=$RUN/bin/kds_server
export SP=/tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1
hoststamp() { # label
  {
    echo "=== $1 $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "loadavg: $(cat /proc/loadavg)"
    echo "cpu pressure: $(head -1 /proc/pressure/cpu 2>/dev/null)"
    echo "competing: $(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | tr '\n' ';')"
    echo "top cpu:"
    ps -eo pid,pcpu,comm --sort=-pcpu | sed -n 2,7p
    echo "df -T datadir: $(df -T $RUN/work | tail -1)"
  } >> $RUN/out/hoststamps.log
}
