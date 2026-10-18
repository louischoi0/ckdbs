#!/usr/bin/env python3
"""Restart cell, corrected detection: B stopped cleanly, started again on its data file; from the first statement,
time to the first publish (plan_stats_generation > 0) and to the warm plan (ANALYZE shows gen > 0 and reordered=1)."""
import json, random, re, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import cc_close_run as H


def main():
    out = []
    for attempt in range(3):
        s = H.Srv("B", fresh=False)
        q = H.GAIN["c1-compare-all-r2872"](random.Random("plan"))
        m0 = s.meta()
        t0 = time.perf_counter()
        first_pub = warm = None
        first_analyze = None
        n = 0
        while time.perf_counter() - t0 < 20:
            s.q(q); n += 1
            el = time.perf_counter() - t0
            g = int(s.meta().get("plan_stats_generation") or 0)
            if first_pub is None and g > 0: first_pub = el
            if first_pub is not None:
                plan = H.analyze(s, q)
                txt = " ".join(plan)
                if first_analyze is None: first_analyze = plan[:4]
                if re.search(r"gen=[1-9]", txt) and "reordered=1" in txt:
                    warm = time.perf_counter() - t0; break
            time.sleep(0.02)
        out.append({"attempt": attempt, "ready_s": round(s.t_ready - s.t_start, 3), "statements_before_publish": n,
                    "first_publish_s": first_pub, "warm_plan_s": warm, "first_analyze_after_publish": first_analyze,
                    "recovery_checkpoint_us": m0.get("recovery_checkpoint_us"), "recovery_analysis_us": m0.get("recovery_analysis_us")})
        s.stop()
        H.save_json("restart2.json", out)
    H.status("RESTART2 DONE")


if __name__ == "__main__":
    main()
