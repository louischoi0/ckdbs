#!/bin/bash
set -e
R=/home/cdkbs/bench-runs/cc-close
D=/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/bench/v3.2.0/archive/cc-close-v3.2.0-184-gc547ef14
rm -rf $D
mkdir -p $D/harness $D/R1 $D/R2 $D/servers
cp $R/harness/*.py $R/harness/*.sh $R/harness/doc_template.md $D/harness/
for run in out:R1 out2:R2; do
    src=${run%%:*}; dst=${run##*:}
    for f in $R/$src/*; do
        b=$(basename $f)
        case $b in
            restart.json|run.pid|extra.pid|extra.done|run.done) continue;;
        esac
        case $b in
            overhead.json|overhead2.json|overhead3.json|gain.json|memo.json|extra.json|extra-uniq.json|gain2.json|gain3.json|gain4.json|stability.json|cold.json)
                gzip -9 -c $f > $dst-tmp-$b.gz && mv $dst-tmp-$b.gz $D/$dst/$b.gz;;
            *) cp $f $D/$dst/;;
        esac
    done
done
for l in A A2 B WA WB BP PA PA2 BL B0 B1 B2 B3 WA0 WA1 WB0 WB1; do
    [ -f $R/run/$l.conf ] && cp $R/run/$l.conf $D/servers/
    [ -s $R/run/$l.stderr ] && cp $R/run/$l.stderr $D/servers/
done
{
    echo "binary A: $R/kds_server_A  $(sha256sum $R/kds_server_A | cut -d' ' -f1)  source binary mtime 2026-10-18 11:55:47 (CC-S0 record)"
    echo "binary B: $R/kds_server_B  $(sha256sum $R/kds_server_B | cut -d' ' -f1)  source binary build-release/kds_server mtime $(stat -c %y /home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/build-release/kds_server)"
    echo "pristine data file: /home/cdkbs/bench-runs/cc-s0/load/pristine.db (CC-S0 loader output; not archived)"
} > $D/binaries.txt
cat > $D/README.md <<'EOF'
Raw files of `bench/v3.2.0/results-cc-close-v3.2.0-184-gc547ef14.md`. No data files or WAL segments.

- `R1/` and `R2/`: the two full runs (harness `cc_close_run.py`; launched by `run.sh` / `run2.sh`, watched by `watch.sh` / `watch2.sh`).
  `R1/` also holds the partial runs that share its directory: `overhead2` and `overhead3` (`overhead2.py`, `overhead3.py`),
  `extra` and `extra-uniq` (`extra_plan_vs_code.py`), `gain2`, `gain3`, `gain4` (`gain2.py`, `gain3.py`, `gain4.py`) and `restart2` (`restart2.py`).
  Big per-block files (per-statement latencies in microseconds) are gzipped.
- `servers/`: the config and stderr of every server that was started.
- `harness/`: the drivers, wrappers, analysis scripts (`render.py`, `final.py`, `analyze*.py`) and `mkdoc.py` with its template, which
  regenerates the results file from these files. Paths inside are the run's absolute paths under `/home/cdkbs/bench-runs/cc-close/`.
EOF
du -sh $D
