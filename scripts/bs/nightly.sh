#!/bin/bash
# BS's unattended runner (instructions/v3.0.0/workorder-bs-test-hardening.md
# BS-S1, BS-Q1 (b)): the methods BS adds, run against `main` once a night on
# this host and reported into a file the operator reads. Nothing here runs
# from scripts/githooks/pre-push (the order's §6.1).
#
#   scripts/bs/nightly.sh            run every step against this tree
#   BS_STEPS="asan noassert" ...     run only those steps
#
# cron runs it through the entry scripts/bs/install-nightly.sh writes, which
# first moves the runner's own clone to the ref it follows ($BS_REF, named in
# the report); run by hand it tests the tree it sits in.
#
# Report: $BS_REPORTS/<date>.md, one line per night in $BS_REPORTS/index.md
# (the order's close bar counts seven consecutive nights there), latest.md
# the newest. A step's verdict is PASS, FAIL or SKIP; a known-noisy cell
# that fails is named under its own heading and still counts as a failure -
# it is never retried here (BS-S1).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BS_REPORTS="${BS_REPORTS:-$ROOT/build-bs-reports}"
# The sanitizer configurations first: `io-sweep` runs noassert's ckdbs-sim and
# `bs-cells` asan's kds_bs_tests, both built by the steps before them.
STEPS="${BS_STEPS:-asan tsan noassert io-sweep bs-cells coverage fuzz difftest}"
# Fresh sim seeds a configuration runs beside the committed ones: the
# release build many, the sanitizer builds - 4 to 5 times slower - a few.
SIM_FRESH="${BS_SIM_FRESH:-16}"
SIM_FRESH_SANITIZED="${BS_SIM_FRESH_SANITIZED:-2}"
mkdir -p "$BS_REPORTS"

DAY=$(date -u +%Y-%m-%d)
REPORT="$BS_REPORTS/$DAY.md"
WORK="$BS_REPORTS/$DAY.d"
mkdir -p "$WORK"
COMMIT=$(git -C "$ROOT" rev-parse --short HEAD)
DESCRIBE=$(git -C "$ROOT" describe --tags 2>/dev/null || echo "$COMMIT")
REF="${BS_REF:-$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)}"
START=$(date -u '+%Y-%m-%d %H:%M UTC')

# The cells BS-S1 names as known-noisy: a failure of one is reported under
# its own heading, never retried and never dropped. The third timed out at
# its 60 s under a host load of ~16 and passed 5 runs in 5 alone (S1, §8).
NOISY='CoreRuntimeTest.APeerListenerServesAReadAndAWriteWithNothingGrantedAndRoutesStop|IdAllocationAcrossCores|SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement'

# A measurement must never share the host with this run (CLAUDE.md, BS-Q1
# (b)): a release server running means a measurement is, so the night is
# skipped and says so rather than skewing it. Checked once, at the start.
#
# A release server started from this repository's checkouts only: the host
# also runs a long-lived kds_server of its own (another project's, built
# elsewhere), which is no measurement and would skip every night.
BUSY="${BS_BUSY_PATTERN:-^/home/cdkbs/ckdbs(/\.claude/worktrees/[^/ ]+)?/build-release/kds_server}"
busy_reason() {
    if pgrep -f "$BUSY" >/dev/null 2>&1; then
        echo "a release kds_server of this repository is running (a measurement)"
    fi
}

verdicts=()
summary=()

step_suite() {
    # step_suite <config>: build, then the hook's range of ctest.
    local cfg=$1 log="$WORK/$1-suite.log" rc
    local t0=$SECONDS
    if ! "$ROOT/scripts/bs/config.sh" "$cfg" build > "$WORK/$cfg-build.log" 2>&1; then
        verdicts+=("| $cfg suite | FAIL | the build failed: \`$WORK/$cfg-build.log\` |")
        summary+=("$cfg=BUILD-FAIL")
        return 1
    fi
    "$ROOT/scripts/bs/config.sh" "$cfg" suite > "$log" 2>&1
    rc=$?
    local line failed noisy
    line=$(grep -E 'tests passed, [0-9]+ tests failed out of' "$log" | tail -1)
    failed=$(sed -n '/The following tests FAILED:/,$p' "$log" | grep -E '^\s+[0-9]+ - ' | sed 's/^\s*//')
    noisy=$(printf '%s\n' "$failed" | grep -E "$NOISY" || true)
    local dt=$(( (SECONDS - t0) / 60 ))
    if [ "$rc" -eq 0 ]; then
        verdicts+=("| $cfg suite | PASS | ${line:-?} (${dt} min) |")
        summary+=("$cfg=PASS")
    else
        verdicts+=("| $cfg suite | FAIL | ${line:-ctest exit $rc} (${dt} min), \`$log\` |")
        summary+=("$cfg=FAIL")
        {
            echo "### $cfg: failed cells"
            echo
            printf '%s\n' "$failed" | sed 's/^/- /'
            if [ -n "$noisy" ]; then
                echo
                echo "Of which known-noisy (BS-S1; not retried):"
                printf '%s\n' "$noisy" | sed 's/^/- /'
            fi
            echo
        } >> "$WORK/details.md"
    fi
}

step_sim() {
    # step_sim <config>: the committed corpus plus $SIM_FRESH date-derived seeds.
    local cfg=$1 log="$WORK/$1-sim.log" rc
    [ -x "$ROOT/build-bs-$cfg/ckdbs-sim" ] || {
        verdicts+=("| $cfg sim | SKIP | no ckdbs-sim built |")
        summary+=("$cfg-sim=SKIP")
        return
    }
    local t0=$SECONDS fresh=$SIM_FRESH
    [ "$cfg" = noassert ] || fresh=$SIM_FRESH_SANITIZED
    BS_SIM_FRESH="$fresh" SIM_ARTIFACTS="$WORK/$cfg-sim-artifacts.txt" \
        "$ROOT/scripts/bs/config.sh" "$cfg" sim > "$log" 2>&1
    rc=$?
    local line dt=$(( (SECONDS - t0) / 60 ))
    line=$(grep -E '^sim: [0-9]+ run' "$log" | tail -1)
    if [ "$rc" -eq 0 ]; then
        verdicts+=("| $cfg sim | PASS | ${line:-?}, $fresh fresh seeds (${dt} min) |")
        summary+=("$cfg-sim=PASS")
    else
        verdicts+=("| $cfg sim | FAIL | ${line:-sim exit $rc} (${dt} min), \`$log\` |")
        summary+=("$cfg-sim=FAIL")
        { echo "### $cfg sim: failing runs"; echo; grep '^FAIL' "$log" | sed 's/^/- /'; echo; } >> "$WORK/details.md"
    fi
}

step_run() {
    # step_run <name> <command...>: PASS on exit 0, its log's last line the detail.
    local name=$1 log="$WORK/$1.log" rc t0=$SECONDS
    shift
    "$@" > "$log" 2>&1
    rc=$?
    local dt=$(( (SECONDS - t0) / 60 )) tail_line
    tail_line=$(tail -n 1 "$log")
    if [ "$rc" -eq 0 ]; then
        verdicts+=("| $name | PASS | ${tail_line} (${dt} min) |")
        summary+=("$name=PASS")
    else
        verdicts+=("| $name | FAIL | ${tail_line} (${dt} min), \`$log\` |")
        summary+=("$name=FAIL")
        { echo "### $name"; echo; grep -E '^FAIL|^MISMATCH|^BOTH|\(Failed\)|\(Timeout\)' "$log" | sed 's/^/- /'; echo; } \
            >> "$WORK/details.md"
    fi
}

: > "$WORK/details.md"
for s in $STEPS; do
    # Asked before every step, not once: the night's steps run for hours,
    # and a measurement started after the first must not share the host.
    busy=$(busy_reason)
    if [ -n "$busy" ]; then
        verdicts+=("| $s and after | SKIP | $busy |")
        summary+=("SKIP")
        break
    fi
    case "$s" in
        asan|tsan|noassert)
            # A failed build leaves the last night's ckdbs-sim behind,
            # which is not this commit's.
            if step_suite "$s"; then
                step_sim "$s"
            else
                verdicts+=("| $s sim | SKIP | the build failed |")
                summary+=("$s-sim=SKIP")
            fi
            ;;
        io-sweep)
            # BS-S3: the Nth operation of each device kind, every committed seed.
            step_run io-sweep python3 -I "$ROOT/scripts/bs/io_sweep.py" \
                "$ROOT/build-bs-noassert/ckdbs-sim" --ops "${BS_SWEEP_OPS:-1500}"
            ;;
        coverage)
            # BS-S2: below the recorded floor fails the night (BS-Q4 (b)).
            step_run coverage "$ROOT/scripts/bs/coverage.sh"
            ;;
        difftest)
            # BS-S6: against the PostgreSQL 18 twin (BS-Q3 (a)), committed
            # seeds and the night's own.
            # Tonight's noassert build is the server; one that failed leaves
            # an older night's behind.
            if [ -x "$ROOT/build-bs-noassert/kds_server" ] &&
                ! printf '%s\n' "${summary[@]}" | grep -q '^noassert=BUILD-FAIL$'; then
                step_run difftest python3 -I "$ROOT/scripts/bs/difftest.py" \
                    "$ROOT/build-bs-noassert/kds_server" --seeds "1,2,3,4,$(date -u +%Y%m%d)"
            else
                verdicts+=("| difftest | SKIP | no kds_server of tonight's build |")
                summary+=("difftest=SKIP")
            fi
            ;;
        fuzz)
            # BS-S5: each target for its budget, from its committed corpus.
            step_run fuzz "$ROOT/scripts/bs/fuzz.sh" "${BS_FUZZ_SECONDS:-600}"
            ;;
        bs-cells)
            # The cells labelled bs-nightly (BS-S3's allocation sweep and
            # after), under asan.
            step_run bs-cells "$ROOT/scripts/bs/config.sh" asan bs-cells
            ;;
        *) verdicts+=("| $s | SKIP | unknown step |"); summary+=("$s=SKIP") ;;
    esac
done

{
    echo "# BS nightly - $DAY"
    echo
    echo "Ran $START to $(date -u '+%Y-%m-%d %H:%M UTC') at \`$COMMIT\` (\`$DESCRIBE\`, ref \`$REF\`), $ROOT."
    echo
    echo "| Step | Verdict | Detail |"
    echo "|---|---|---|"
    printf '%s\n' "${verdicts[@]}"
    echo
    cat "$WORK/details.md"
    echo "Logs: \`$WORK/\`"
} > "$REPORT"
ln -sf "$DAY.md" "$BS_REPORTS/latest.md"
echo "- $DAY \`$COMMIT\` ($REF) ${summary[*]}" >> "$BS_REPORTS/index.md"

case " ${summary[*]} " in
    *FAIL*|*SKIP*) exit 1 ;;
esac
exit 0
