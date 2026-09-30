R=/home/cdkbs/bench-runs/pg18-f2f1ee7
WT=/home/cdkbs/ckdbs/.claude/worktrees/bench-pg-floor
{
echo "## date"; date -u +%FT%TZ
echo "## git"; git -C $WT rev-parse --short HEAD; git -C $WT describe --tags; git -C $WT branch --show-current; git -C $WT status --short | head
echo "## git diff --stat 9a0525d HEAD -- src include CMakeLists.txt tools"; git -C $WT diff --stat 9a0525d HEAD -- src include CMakeLists.txt tools
echo "## df -T"; df -T /home/cdkbs /home/cdkbs/pg-bench-18 /home/cdkbs/bench-runs /tmp
echo "## lscpu"; lscpu | grep -E "Model name|^CPU\(s\)|Thread|Core|Socket"; uname -r
echo "## kds binary (copy used for the --bookers 1 cells)"; cat $R/binary.sha256
stat -c '%y %n' /home/cdkbs/bench-runs/rebaseline-9a0525d/kds_server $R/kds_server
echo "source binary mtime recorded by the 9a0525d run:"; cat /home/cdkbs/bench-runs/rebaseline-9a0525d/source-binary-mtime.txt
} > $R/stamp.txt 2>&1
cat $R/stamp.txt
