#!/bin/bash
# Re-check every done cell against the monitor; move flagged ones to contaminated/ and clear their done marker.
RUN=/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e
cd "$RUN"
for t in $(ls done); do
    p=$t
    [ -e "logs/$t.precheck" ] || p=$t-off
    if ! python3 analysis/cellvalid.py "$t" "$p" >> logs/revalidate.log 2>&1; then
        tail -1 logs/revalidate.log
        mkdir -p "contaminated/$t.attempt0"
        mv logs/$t.* logs/$t-*.* json/$t.json conf/$t.conf conf/$t-*.conf "contaminated/$t.attempt0/" 2>/dev/null
        rm -f "done/$t"
        echo "== $t CONTAMINATED (revalidate) ==" >> timeline
    fi
done
ls done | wc -l
