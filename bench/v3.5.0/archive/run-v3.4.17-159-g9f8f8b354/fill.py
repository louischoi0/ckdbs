"""Fill the document templates' @@PLACEHOLDER@@ prose, then expand {{fragments}} and write the
five documents. Usage: python3 -I fill.py <repo-root>"""
import glob, json, os, re, sys

R = "/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354"
D = "v3.4.17-159-g9f8f8b354"
OUT = sys.argv[1] + "/bench/v3.5.0"


def vcount(pat):
    ok = n = 0
    for f in glob.glob(f"{R}/out/json/{pat}"):
        d = json.load(open(f)); n += 1
        ok += d["meta"].get("verify_problems") == []
    return ok, n


s1ok, s1n = vcount("*s1*-c*.json")
s2 = [json.load(open(f))["meta"] for f in glob.glob(f"{R}/out/json/*s2-*.json")]
aband = sum(m["abandoned"] for m in s2)
PINW = "21:52:11 to 22:07:44 UTC"

P = {}
P["PIN_PAIRS"] = "five"
P["PIN_WINDOW"] = PINW
P["S4R_WINDOW"] = "22:07:46 to 22:12:20 UTC"

P["S1_ONE_LINE"] = (
    "On scenario 1 the engine at `9f8f8b354` is the engine of `07f4552a` (`v3.5.0`): in the same-host A/B with the server pinned to "
    "the P-cores (five interleaved pairs), every warm, cold and Cabin shape that resolves reads 0.98x to 1.02x and every phase that "
    "resolves 0.95x to 1.02x, all inside their spreads. Nothing in DL, DK, DD, DN, DO, DP, DQ, DR or DU has a price scenario 1 can "
    "read. The cells themselves read 1.5x to 2.3x the `v3.5.0` run's (the phases 1.3x to 3.2x), which is this host against that one. "
    "Unpinned, this host does not resolve an engine delta: the three-run spread of a warm shape at `c1-g` is 12.5 % to 79.1 %. "
    f"Verification passed {s1ok} of {s1n} runs (main, A/B and pinned A/B).")
P["S1_ENGINE"] = (
    "- **No change between `07f4552a` and `9f8f8b354` on scenario 1 where the pinned A/B resolves**: ranges, walks, the three-relation "
    "join, the model join, the Cabin-served `symbol-history` and `model-join`, the aggregates and the bulk loads read 0.95x to 1.02x, "
    "12 of the 18 sweep rows and 18 of the 23 phases inside their spreads and none marked.\n"
    "- **Not resolved, and not read**: the statements under about 100 µs (`bar-lookup` and `point-join`, cold and warm, new-arm "
    "spreads 28.9 % to 66.9 %) and the two Cabin-served shapes above 10,000 qps (`day-slice` cabin 0.91x, `cross-join` cabin 0.85x, "
    "new-arm spreads 52.7 % and 20.3 %; the unpinned series reads them 1.02x and 0.98x). A row over 20 % carries no claim by this "
    "directory's rule; five pairs do not settle the two Cabin rows either way.\n"
    "- Also not resolved: `result-insert` (0.99x, the new arm's five runs spread 50.3 %), and the phases of one to eight statements "
    "(`load-exchanges`, `load-symbols`, `load-models`, `sync`).\n"
    "- The ratios against the `v3.5.0` run (1.5x to 2.3x on the warm shapes, 2.57x on `result-insert`, 2.11x on `load-bars`) price "
    "this workstation against that VM, not the 142 commits between the two.")
P["S1_PIN_CONTAM"] = "No pinned A/B cell of scenario 1 carries such a sample."
P["S1_PIN_GATE"] = "; pinned A/B: `pin-ab-old1-s1-c1-g` 40 s, the rest 0 s"

P["S2_ONE_LINE"] = (
    "On the booking path the engine at `9f8f8b354` is the engine of `07f4552a` (`v3.5.0`): in the pinned same-host A/B (five "
    "interleaved pairs) one booker at one core, `strict`, reads 1,749.7 tps against 1,749.5 tps (1.00x, spreads 1.1 % and 0.9 %) with "
    "every statement of the transaction at 1.00x to 1.02x. Eight bookers read 1.01x at one core and 1.03x at eight, neither a claim, "
    "because each has one stalled run in its ten (526.1 tps in the old arm at one core, 4,149.1 tps in the new arm at eight; no "
    "foreign CPU in the monitor). At eight cores the three write statements read 1.05x each, `org-update` marked by the spread rule "
    "(+4.6 % against 4.2 %). The matrix reads 2.1x to 3.2x the `v3.5.0` run's, which is the host. The invariant failures under more "
    "than one booker are the driver's client-side lost update, as before: 0 of 400 with one booker and 0 of 400 under repeatable-read. "
    f"One booking in this run's {len(s2)} scenario 2 cells was abandoned at `--max-retries 5` ({aband} in all).")
P["S2_ABANDON"] = (f"across all {len(s2)} scenario 2 cells of this run (main, extras and both A/B series, old binary and new) it is "
                   f"the only one ({aband} in all).")
P["S2_ENGINE"] = (
    "- **No change the pinned A/B can read at one core.** One booker, `strict`: 1.00x on tps, 1.00x to 1.02x on the booking and its nine "
    "statements, every one inside its spread (the three reporter reads, 10 to 30 statements a run, do not resolve). Eight "
    "bookers, `group`: 4,228.0 tps against 4,192.8 tps (1.01x); the old arm's fifth run stalled (526.1 tps), which puts every row of "
    "that cell over 20 %, and the other nine runs lie between 3,991.7 and 4,257.2 tps.\n"
    "- **At eight cores the A/B leans 3 % to 5 % toward the new engine, without a claim.** 9,801.8 tps against 9,543.2 tps (1.03x); "
    "the new arm's fifth run stalled (4,149.1 tps), so the rule gives no claim; the new arm's other four runs are 9,593.5 to 9,874.0 "
    "tps and the old arm's five 9,288.8 to 9,661.8 tps. `charge-insert`, `operation-update` and `org-update` read 1.05x each, the "
    "last marked (+4.6 % against spreads of 4.2 % and 3.3 %). It is not attributed to a milestone: the unpinned series cannot see it "
    "(1.06x, spread 51.3 %), and at one core the same statements read 1.00x to 1.02x.\n"
    "- **Two stalled runs, cause unverified.** `pin-ab-old5-s2-c1-g` (old engine): booking max 2,176.4 ms, commit max 805.9 ms, "
    "`charge-insert` max 523.1 ms. `pin-ab-new5-s2-c8-g` (new engine): commit p95 10.8 ms against a p50 of 0.2 ms. Neither cell has a "
    "foreign CPU sample or a gate wait. The monitor samples CPU only, so a device stall from another process is not excluded; the "
    "check is an I/O sampler (`iostat -x 1`) beside the cells. One stall is in each engine, so the pair says nothing about "
    "either.\n"
    "- **An insert can be refused by a descent that gave up, on both engines.** At eight cores the server logs `btree descent for "
    "key <k> from page <p> gave up after 5 attempts: each leaf it reached had already given the key away` and answers the `INSERT` "
    "`TXN_CONFLICT retryable=1`, on `charges` (29 of the 37) and `freights` (8) at keys 107 to 138: 24 such refusals over the old "
    "arm's eight eight-core A/B cells and 13 over the new arm's eight. The driver retries them and they are inside `conflicted`. "
    "Counts this small carry no claim between the engines.\n"
    "- The cross-host ratios (2.06x to 3.15x on the matrix, 2.23x to 3.52x on the booker sweep) are the host.")
P["S2_PIN_CONTAM"] = ""
P["S2_PIN_GATE"] = "; pinned A/B: `pin-ab-new1-b1-s2-c1-s` 46 s, `pin-ab-new1-s2-c8-g` 6 s, the rest 0 s"

P["S3_ONE_LINE"] = (
    "On scenario 3 the engine at `9f8f8b354` is the engine of `07f4552a` (`v3.5.0`): in the pinned same-host A/B (five interleaved "
    "pairs, 10,000 loans) the twelve read shapes read 0.97x to 1.03x unindexed and 0.96x to 1.01x indexed, 22 of the 24 inside their "
    "spreads, one over 20 % (`resv-by-user` indexed, 1.00x) and one marked by the rule at the floor: `count-by-user` unindexed, "
    "-1.1 % against spreads of 1.0 % and 0.6 %. The unpinned A/B reads 0.58x to 0.80x on the indexed cell, and that is core "
    "placement, not the engine (section 5). The cells read 1.4x to 3.8x the `v3.5.0` run's, which is the host. Verification passed "
    "in every run and `--assert-index-reads` in every indexed cell.")
P["S3_ENGINE"] = (
    "- **No change between `07f4552a` and `9f8f8b354` on the library's reads**: the tuple key lookup, the unindexed walks, the "
    "indexed probes and ranges, the two joins, the correlated `EXISTS` and the grouped count read 0.96x to 1.03x pinned, and the "
    "online index build (`create-index`, 1.02x) is inside its spread.\n"
    "- **One row is marked, at 1.1 %**: `count-by-user` unindexed, 7,418.4 qps against 7,501.9 qps, with five-run spreads of 1.0 % "
    "and 0.6 %. The same shape indexed reads 0.99x inside its spread (3.1 % and 2.1 %). The rule marks it and it is reported as "
    "marked; a 1.1 % reading on a 135 µs statement is not attributed to a milestone here.\n"
    "- `ddl` and `load` do not resolve (spreads 38 % to 168 %): four and 19,000 statements whose mean is set by a few slow ones.\n"
    "- The cross-host ratios (1.40x to 3.81x at one core, 1.59x to 3.73x at eight) are the host.")
P["S3_PIN_CONTAM"] = "No A/B cell, pinned or not, carries one."
P["S3_PIN_GATE"] = ("; pinned A/B: `pin-ab-new1-s3-c1-n10000-none` 406 s in two holds (another session's test suite), "
                    "`pin-ab-new1-s3-c1-n10000-all` 24 s, the rest 0 s")

P["S4_ONE_LINE"] = (
    "**The `on` arm is still the walk, as at `v3.5.0`, and that is the contract** (`cabin-controller.md` PO11): in four runs on two "
    "binaries the controller creates five or six Cabins on heat, each holds one page, records no lookup and is never extended, "
    "because no statement's step probes a controller-made Cabin without an administrator's plan naming it, and this driver writes "
    "none. The `declared` arm serves: 1.65x to 2.35x over the walk on `board` and 1.66x to 2.21x on `tape10k`. **On this host `on "
    "over off` is not level on `board`**: 1.08x to 1.25x in mean latency on all twelve days of the four runs, the `07f4552a` "
    "binary's among them, with the two arms' p50 within 5 % of each other in the measured binary's runs. It is a property of this "
    "host under this driver's pacing, the same on both binaries, and not a Cabin serving. New over old on this host is one run "
    "against one and scatters 0.81x to 1.22x on the probe groups, without a claim. The 18-statement byte comparison passed on every "
    "day of every run.")
P["S4_ENGINE"] = (
    "- **PO11 holds at `9f8f8b354` exactly as at `07f4552a`**: `creates` 5 (run A, both times, and the old binary) or 6 with 2 drops "
    "(run B), `extends` 0, one page a Cabin, both lookup shares NULL in all 27 snapshots of every run. To measure the controller the "
    "driver would need a `CREATE PLAN ... ACCESS <relation> CABIN (<column>)` per probed shape, which is not built.\n"
    "- **A declared Cabin serves**, at `board` 1.65x to 2.35x and at `tape10k` 1.66x to 2.21x over the walk across the measured "
    "binary's three runs (the `07f4552a` binary on this host: 1.84x to 2.11x and 1.67x to 2.16x). Server CPU on the boards is 1.2 s "
    "to 1.3 s for `off`, 1.1 s for `on` and 0.5 s to 0.6 s for `declared` over three days in each run, the same on the old binary (1.2 s, 1.1 s, "
    "0.5 s).\n"
    "- **No engine reading beyond that.** One run against one run: the probe groups scatter 0.81x to 1.22x new over old with no "
    "pattern by arm or day, and the `pk` control's own `on over off` runs 0.86x to 1.19x.\n"
    "- **`on over off` above 1 on `board` is not the controller.** It appears on both binaries, with no lookup recorded on either. "
    "Its cause is unverified; the check is the per-block latency of each arm by its position in the driver's interleave.")
P["S4_RERUN"] = ("the run was made again as `r2-s4` (22:07:46 to 22:12:20 UTC), which carries no such sample and reads the same way "
                 "(`on over off` on `board` 1.21x, 1.08x, 1.25x against 1.12x, 1.17x, 1.16x; the controller's final state the same: "
                 "five Cabins of one page, `extends` 0).")
P["S4_GATE"] = "0 s at every scenario 4 run."

P["SUM_ONE_LINE"] = (
    "**On these four workloads the engine at `9f8f8b354` is the engine of `v3.5.0` (`07f4552a`).** In the same-host A/B with the "
    "server pinned to the P-cores (five interleaved pairs), every shape and phase of scenarios 1 and 3 that resolves reads 0.95x to "
    "1.03x, scenario 2's single-booker transaction reads 1.00x with every statement at 1.00x to 1.02x, and the eight-booker cells "
    "read 1.01x (one core) and 1.03x (eight cores) without a claim. Two rows are marked by the spread rule, both at the floor: "
    "scenario 3's `count-by-user` unindexed (-1.1 %) and scenario 2's `org-update` at eight cores (+4.6 %). DL, DK, DD, DN, DO, DP, "
    "DQ, DR and DU have no price these workloads can read. **The cells themselves read 1.3x to 3.8x the `v3.5.0` run's, and that is "
    "the host**: this run is on a 24-CPU hybrid workstation, that one on an 8-CPU VM. **Scenario 4's `on` arm is still the walk** "
    "(PO11: five or six one-page Cabins, no lookup recorded, `extends` 0) on both binaries, and a declared Cabin serves 1.65x to "
    "2.35x on `board`. Every byte comparison and every verification passed. PostgreSQL was not measured: this host has no server. "
    "**Unpinned, this host is a poor instrument**: a hybrid CPU under `powersave` with another session building and testing "
    "throughout; three-run spreads of 20 % to 80 % are common and the unpinned A/B resolves almost nothing.")
P["SUM_TIMELINE"] = (
    "fetch and reading the `v3.5.0` run 20:58:08 to 20:59; build 20:59:12 to 21:00:04; a first launch 21:02:43 to 21:03:49, stopped "
    "and discarded (section Watchdog); scenario 1 21:05:49 to 21:09:38; scenario 2 to 21:11:56; scenario 3 to 21:15:08; scenario 4 "
    "run A 21:15:10 to 21:19:43; the unpinned A/B 21:19:44 to 21:41:59 (1,035 s of it held at two gates); the `07f4552a`-binary "
    "scenario 4 run 21:42:01 to 21:46:34; extras 21:46:35 to 21:46:54; scenario 4 run B 21:46:55 to 21:51:28; the pinned A/B "
    "21:52:11 to 22:07:44 (522 s held); scenario 4 run A again 22:07:46 to 22:12:20. **74 minutes of wall clock, 28 minutes of it "
    "quiet-gate hold (1,691 s), so 46 minutes inside the budget of about one hour.** 186 cells, every driver exit 0.")
P["SUM_BUDGET"] = (
    "46 minutes, building included, outside 28 minutes of gate hold (`CLAUDE.md`, 2026-11-18: the time a quiet gate holds a "
    "measurement is outside the budget). **Cut**, as at `v3.5.0`: the extreme cells of scenario 2 (20,000 bookings, 32 bookers, one "
    "and two client processes), scenario 1's join-order probe, scenario 3's plan replies, scenario 4's six-run interleaved A/B, and "
    "PostgreSQL. **Added** to the `v3.5.0` shape: an eight-core booking cell and an indexed library cell in the A/B; the pinned A/B "
    "series (60 cells); scenario 4's run A a second time")
P["SUM_HEADLINES"] = "{{sum-headlines}}"
P["SUM_ENGINE_TABLE"] = ("{{sum-engine}}\n\nScenario 4, ratio of mean latencies over the `off` arm, days 1 to 3 (the last column is the "
                         "`v3.5.0` run on its own host):\n\n{{sum-s4}}")
P["SUM_MOVERS"] = (
    "- **Nothing moved in the engine between `07f4552a` and `9f8f8b354` that scenarios 1 to 3 can read** (DL's statistics paths, "
    "DK's write descent, DD's index purge, DN's heap, DO's frame table holds, DP's views, DQ's Cabin miss, DR's primary keys, DU's "
    "keyed writes; superblock 38 to 41). The pinned A/B is level on every row it resolves.\n"
    "- **Two rows are marked, at the floor.** Scenario 3 `count-by-user` unindexed, -1.1 % against spreads of 1.0 % and 0.6 % (its "
    "indexed twin 0.99x, inside). Scenario 2 `org-update` at eight cores, +4.6 % against 4.2 %, beside `charge-insert` and "
    "`operation-update` at 1.05x inside their spreads and a tps of 1.03x without a claim. Neither is attributed to a milestone.\n"
    "- **Not resolved**: scenario 1's statements under about 100 µs and its two Cabin-served shapes above 10,000 qps (`day-slice` "
    "cabin 0.91x, `cross-join` cabin 0.85x pinned, new-arm spreads over 20 %; 1.02x and 0.98x unpinned). Five pairs do not settle "
    "them.\n"
    "- **Two stalled runs in the pinned scenario 2 cells**, one in each engine (526.1 tps where the other nine runs read 3,991.7 to "
    "4,257.2 tps; 4,149.1 tps where the others read 9,288.8 to 9,874.0 tps), with no foreign CPU sample. Cause unverified; the "
    "monitor does not sample I/O.\n"
    "- **Scenario 4 reads as at `v3.5.0`**: the controller's Cabins are unprobed (PO11) and a declared Cabin serves. `on over off` "
    "on `board` is 1.08x to 1.25x here on both binaries with no lookup recorded on either, so it is this host, not a Cabin.\n"
    "- **Both engines refuse a few inserts `TXN_CONFLICT` at eight cores from a btree descent that gave up after 5 attempts** (24 "
    "over the old arm's eight eight-core A/B cells, 13 over the new arm's); the driver retries them.\n"
    f"- **One abandoned booking** in {len(s2)} scenario 2 cells (`r2-s2-c8-s`, out of retries at `--max-retries 5`).")
P["SUM_GATE"] = (
    "Of the 186 cells, 173 started with no wait and 13 were held, 1,691 s in all: 134 s over six cells of the main run, 1,035 s over "
    "two of the unpinned A/B (`ab-new1-s2-c1-g` 720 s, `ab-old2-s1-c1-g` 315 s) and 522 s over five cells of the pinned A/B's first "
    "pair. 95 cells started under the first rule and 91 under the second; 41 of the 91 started with an idle build or test process "
    "present. The gate guards a cell's start only: section Contamination lists what ran inside one.")
P["SUM_WATCHDOG"] = (
    "No watchdog found an orphan or a stall: `watchdog.sh` logged 45 lines of one-minute checks, `watchdog2.sh` and `watchdog3.sh` the rest, "
    "and none logged `ORPHANED`. The run was stopped and restarted by hand three times, each time between cells or at a gate, and "
    "each restart resumed from the per-cell `.rc` files:\n\n"
    "1. **21:03:49 UTC: the first launch (three cells, from 21:02:43) was stopped and discarded whole.** Its scenario 2 cell would "
    "have run one invocation where the `v3.5.0` run ran three (`--schema-only`, `--load-only`, the run), its size sweep lacked "
    "`--exchanges`, and it had no foreign-process monitor. The `v3.5.0` run's own scripts (`bench/v3.5.0/archive/run-v3.5.0/`) were "
    "read at that point and the runner rewritten to their cells, arguments and suffixes. Nothing of the first launch is in any "
    "table; its status file is archived as `logs/attempt1-status`.\n"
    "2. **21:38:36 UTC: stopped at a gate to change the gate's rule** (section Quiet gate).\n"
    "3. **21:59:38 UTC: the pinned series was stopped at a gate** so that its deadline would follow the holds inside it.\n\n"
    "The pinned series and scenario 4's second run A were queued behind the main run as separate scripts (`run_pin.sh`, "
    "`run_s4r.sh`), each with its own watchdog.\n\n"
    "**Disclosure.** At 21:59 UTC this session ran `ctest -N` against the other session's build directory "
    "(`.claude/worktrees/dt-write-path-yield/build`) to count the suite holding the gate (5,286 tests). It ran no test, but that "
    "directory's `Testing/Temporary/LastTest.log` was 121 bytes afterwards, so the listing appears to have replaced the log of the "
    "suite then running there.")
P["SUM_CONTAM"] = (
    "No cell was discarded. A monitor sampled the host about every 2 s for foreign processes above 10 % of one CPU. Seven cells "
    "carry samples of 50 % or more of one CPU (the `claude` CLI's own processes not counted), all from the other session's build "
    "and test cycle in the worktree `dt-write-path-yield`, kept and disclosed:\n\n"
    "- **scenario 4 run A (`s4`)**: 43 of 123 samples, 8.3 CPUs' worth on average (a full build). Run again as `r2-s4`, which "
    "carries none.\n"
    "- **scenario 4 run B (`cd2-s4`)**: 19 of 122 samples, `kds_tests` at about one CPU.\n"
    "- `s1-c8-g` run 1 (5 of 8 samples, `cc1plus` and `ld`), `ab-new1-s1-c1-g` (2 of 9, `cc1plus`), `r2-s3-c1-n10000-none` (2 of "
    "2), `r2-s3-c8-n10000-all` (2 of 3) and `r3-s3-c8-n10000-none` (3 of 3; `cc1plus`).\n\n"
    "One sample inside `s2-c1-g` run 1 shows six `python3 -c` processes (pids 629971 to 629977) at 10.9 % each: the scenario 2 "
    "driver's own booker processes, which the monitor's filter did not name. They are not foreign and `contam.py` leaves them out. "
    "No scenario 2 cell, no pinned A/B cell and not the `07f4552a`-binary scenario 4 run carries a foreign one. **What the monitor does not see "
    "is I/O**: the two stalled cells of the pinned series (scenario 2, section 9) show no foreign CPU, and a device stall from "
    "another process is not excluded. **Disclosure:** the table generator ran and the documents were drafted by this session while "
    "cells were live, so `claude` processes are in the monitor at up to 38.6 % of one CPU (87 samples above 10 % in 1,784); they "
    "are excluded from the counts above.")

FIX = {
    "results-scenario4-cabinopt-days": [
        ("(1.12x to 1.24x in means in run A, run B and the `07f4552a` control alike), while the two arms' p50 agree to 3 µs in 200 µs on every day",
         "(1.08x to 1.25x in means over the four runs, the `07f4552a` control among them), while the two arms' p50 are within 5 % of each other in the measured binary's three runs (the control's day 1: 218.3 µs against 202.0 µs)"),
        ("The difference is in the `off` arm's tail, the arm each block probes first; the controller's",
         "The difference is in the `off` arm's tail; the controller's"),
        ("The `pk` control's `on over off` runs 0.91x to 1.04x in run A, the floor a ratio is read against.",
         "The `pk` control's own `on over off` runs 0.86x to 1.19x over the four runs, so the floor on this host is about 20 % either way; the board's ratio is inside or just above it, but above 1 on all twelve days."),
        ("{{s4-meta-ab-old1-s4}}", "Its final snapshot: {{s4-meta-ab-old1-s4}}"),
    ],
}


FIX["results-scenario1-backtest"] = [
    ("`verify_problems` empty: {{s1-verify}}", f"`verify_problems` empty in {s1ok} of {s1n} runs (18 main, 6 unpinned A/B, 10 pinned A/B)"),
    ("The unpinned spreads here are 3 to 15 times the `v3.5.0` run's on the same cells", "The unpinned spreads here are 2 to 14 times the `v3.5.0` run's on the same cells"),
    ("the server's thread lands on a P-core or an E-core per run, and the two differ by about 1.5x.", "nothing fixes which class of core the server's thread runs on (section 9), and section 7 repeats the series with that fixed."),
]
FIX["results-scenario3-library"] = [
    ("is the ratio of the cores the two arms' servers ran on (the P-cores against the E-cores, about 1.5x apart), which is what section 6 removes.",
     "is not a property of any one access path. Section 6, which fixes where the server runs, reads the same cells 0.96x to 1.01x. The likeliest cause is the class of core the unpinned servers' threads ran on; it is unverified, since no per-run placement was recorded."),
]
FIX["summary-scenarios"] = [
    ("A data file of one binary does not mount on the other; every cell creates its own", "The superblock moved from 38 to 41 between them (each binary's start-up line names its own); every cell creates its own data file"),
]


FIX["results-scenario2-freight"] = [
    ("- One main cell carries a foreign sample of 50 % or more of one CPU: `s2-c1-g` run 1 (another session's `python3`, 65 %, one sample of two, 21:09:48 to 21:09:51 UTC); kept and disclosed. ",
     "- **No scenario 2 cell carries a foreign sample of 50 % or more of one CPU**: not the main cells, the extras or either A/B series. (One sample inside `s2-c1-g` run 1 shows six `python3 -c` processes at 10.9 % each; they are this driver's own booker processes, `multiprocessing` children.)"),
]


# ---- corrections after the critics-developer review (2026-11-26 22:24 UTC), applied to the runtime strings ----
def fixp(key, a, b):
    if a not in P[key]:
        print("OVERRIDE NOT FOUND", key, ":", a[:60])
    P[key] = P[key].replace(a, b)


fixp("SUM_ONE_LINE", "Every byte comparison and every verification passed.",
     "Every byte comparison passed, and every verification of scenarios 1 and 3; scenario 2's invariant check fails under more than "
     "one booker as at `v3.5.0` (the driver's client-side lost update) and passes 400 of 400 in every single-booker and "
     "repeatable-read cell.")
fixp("SUM_ONE_LINE", "three-run spreads of 20 % to 80 % are common and the unpinned A/B resolves almost nothing.",
     "three-run spreads of 20 % to 80 % are common, most unpinned A/B rows fail the spread rule, and of the eight rows the rule "
     "marks unpinned the pinned series confirms none (seven read 0.95x to 1.02x inside their spreads, one carries no claim).")
fixp("S1_ONE_LINE", "The cells themselves read 1.5x to 2.3x the `v3.5.0` run's (the phases 1.3x to 3.2x)",
     "The warm shapes read 1.5x to 2.5x the `v3.5.0` run's (the phases 1.3x to 3.2x)")
fixp("S1_ONE_LINE", "Unpinned, this host does not resolve an engine delta: the three-run spread of a warm shape at `c1-g` is 12.5 % to 79.1 %.",
     "Unpinned, the A/B on this host is not usable: 15 of its 18 sweep rows and 14 of its 23 phases fail the spread rule, and the six "
     "rows the rule marks (1.05x to 1.51x) read 0.95x to 1.02x pinned, inside their spreads.")
fixp("S1_ENGINE", "(1.5x to 2.3x on the warm shapes,", "(1.49x to 2.48x on the warm shapes,")
P["S1_ENGINE"] += (
    "\n- **The unpinned series marks six rows, and the pinned series confirms none.** `cross-join` cold 1.05x, `read-day-slice` "
    "1.24x, `read-join-point` 1.35x, `read-join-exists` 1.38x, `agg-global` 1.51x and `agg-by-symbol` 1.41x unpinned read 1.02x, "
    "1.00x, 0.95x, 1.00x, 0.99x and 0.99x pinned, each inside its spread. Three runs an arm do not sample where an unpinned "
    "server's thread runs on this host, so a tight unpinned spread is not a small error; the marks are reported and not read as "
    "engine changes.")
P["S2_ENGINE"] += (
    "\n- The unpinned series marks one row, `load-cargos` in the eight-booker one-core cell (1.02x against spreads of 1.4 % and "
    "1.3 %). Pinned, that row carries no claim (1.12x, the old arm's stalled run inside it) and reads 0.99x in the single-booker "
    "cell and 1.05x, without a claim, at eight cores. It is not read as an engine change.")
fixp("S3_ONE_LINE", "The unpinned A/B reads 0.58x to 0.80x on the indexed cell, and that is core placement, not the engine (section 5).",
     "The unpinned A/B reads 0.58x to 0.80x on the indexed cell's twelve read shapes, which the pinned series does not confirm "
     "(section 5).")
P["S3_ENGINE"] += (
    "\n- The unpinned series marks one row, `loans-by-daterange` indexed (0.80x), inside a table that reads 0.58x to 0.80x on every "
    "read shape; pinned, that row reads 1.01x inside its spread. It is not read as an engine change.")
fixp("S4_ONE_LINE", "It is a property of this "
     "host under this driver's pacing, the same on both binaries, and not a Cabin serving.",
     "It is the same on both binaries with no lookup recorded on either, so it is not a Cabin serving and not a change between the "
     "two engines; the `07f4552a` binary read 0.99x to 1.00x on the `v3.5.0` run's host (one run there, one here), so the ratio "
     "follows the host, and what on this host produces it is unverified.")
fixp("S4_ENGINE", "It appears on both binaries, with no lookup recorded on either. "
     "Its cause is unverified;",
     "It appears on both binaries, with no lookup recorded on either, and the `07f4552a` binary's own run A on the `v3.5.0` run's "
     "host read 0.99x, 1.00x and 1.00x (section 6's last columns): the ratio follows the host. What on this host produces it is "
     "unverified;")
fixp("SUM_MOVERS", "so it is this host, not a Cabin.",
     "so it is not a Cabin and not a change between the engines; the `07f4552a` binary read 0.99x to 1.00x on the `v3.5.0` run's "
     "host, so the ratio follows the host, and its cause here is unverified.")
fixp("SUM_CONTAM", "Seven cells "
     "carry samples of 50 % or more of one CPU (the `claude` CLI's own processes not counted)",
     "Seven cells "
     "carry samples in which those processes sum to 50 % or more of one CPU (the `claude` CLI's own processes not counted)")
fixp("SUM_CONTAM", "(87 samples above 10 % in 1,784)", "(87 entries in 86 of the 1,784 samples)")

FIX["results-scenario1-backtest"].append((
    "The unpinned spreads here are 2 to 14 times the `v3.5.0` run's on the same cells (median spread of the `c1-g` warm shapes 22.7 % here against 4.9 % there), and `point-join` warm spreads 79.1 %.",
    "At `c1-g` the unpinned warm spreads are 2 to 14 times the `v3.5.0` run's (median 22.7 % here against 4.9 % there; `point-join` 79.1 %); at `c1-s` and `c8-g` some shapes spread no more than they did there."))
FIX["results-scenario2-freight"].append((
    "the client-side lost update its source names. It is not engine corruption.",
    "the client-side lost update its source names. 54 of this run's 80 scenario 2 cells report a failure: 53 with `I1` first and one (`x-b2-s2-c8-g`) with `I3 organization <n>: outstanding=..., recomputed from its freights and charges=...`, which is consistent with the same lost update on `org-update`, a statement that also writes a literal (`SET outstanding = <literal>`). Every single-booker cell and both repeatable-read cells report 0. It is not engine corruption."))
FIX["results-scenario3-library"] += [
    ("A shape is 200 statements, 7 ms to 66 ms of work on this host at 10,000 loans unindexed",
     "A shape is 200 statements, 7 ms to 124 ms of work on this host at 10,000 loans unindexed"),
    ("**The indexed table is a placement reading, not an engine reading.** Its new-over-old ratios sit at 0.58x to 0.80x on every shape at once, the primary-key lookup (`pk-user`, 0.62x) and the load among them, and every row but one fails the spread rule.",
     "**The indexed table is not an engine reading.** Its new-over-old ratios sit at 0.58x to 0.80x on all twelve read shapes at once, the primary-key lookup (`pk-user`, 0.62x) among them, and every row but one fails the spread rule (`loans-by-daterange`, 0.80x, is marked)."),
]
FIX["results-scenario4-cabinopt-days"].append((
    "One run of the driver at the defaults on the `07f4552a` binary (`ab-old1-s4`):",
    "One run of the driver at the defaults on the `07f4552a` binary (`ab-old1-s4`). The last two columns are the `v3.5.0` run's own run A (`s4`), the same binary and settings on the other host:"))
# ---- end of the review's corrections ----

for name in ("results-scenario1-backtest", "results-scenario2-freight", "results-scenario3-library",
             "results-scenario4-cabinopt-days", "summary-scenarios"):
    t = open(f"{R}/docs-src/{name}.md").read()
    for a, b in FIX.get(name, []):
        if a not in t:
            print("FIX NOT FOUND in", name, ":", a[:60])
        t = t.replace(a, b)
    left = set(re.findall(r"@@([A-Z0-9_]+)@@", t))
    for k in left:
        if k not in P:
            print("NO TEXT for", k, "in", name)
    t = re.sub(r"@@([A-Z0-9_]+)@@", lambda m: P.get(m.group(1), m.group(0)), t)
    missing = []

    def sub(m):
        try:
            return open(f"{R}/out/tables/{m.group(1)}.md").read().rstrip("\n")
        except OSError:
            missing.append(m.group(1)); return "(fragment missing: " + m.group(1) + ")"
    t = re.sub(r"\{\{([a-z0-9A-Z_-]+)\}\}", sub, t)
    out = f"{OUT}/{name}-{D}.md"
    open(out, "w").write(t)
    print(os.path.basename(out), len(t.splitlines()), "lines; missing fragments:", missing or "none",
          "; leftovers:", re.findall(r"@@[A-Z0-9_]+@@|\{\{[^}]+\}\}", t) or "none")
print("s1 verify", s1ok, s1n, "; s2 cells", len(s2), "abandoned", aband)
