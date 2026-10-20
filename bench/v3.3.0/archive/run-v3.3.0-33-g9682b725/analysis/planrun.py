"""Run scenario1 or scenario3 once with the full ANALYZE replies of the join shapes dumped to a file.
usage: planrun.py s1|s3 <dump-file> <driver args...>   (tools/ on the path)"""
import sys, json
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4/tools")
which, dump = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
if which == "s3":
    import scenario3_library as m
    orig = m.analyze_shapes

    def wrap(exec_, tables, users, books):
        r = orig(exec_, tables, users, books)
        json.dump({k: v["raw"] for k, v in r.items()}, open(dump, "w"), indent=1)
        return r
    m.analyze_shapes = wrap
    sys.argv = ["scenario3_library.py"] + args
    m.main()
else:
    import scenario1_backtest as m
    orig = m.print_plans

    def wrap(client, suffix, models, session_count, bar_count):
        orig(client, suffix, models, session_count, bar_count)
        out = {}
        for name, sql in (("backtest-read", m.cross_section_sql(suffix, session_count // 2)),
                          ("compare-all", m.compare_all_sql(suffix)),
                          ("compare-one (model-join)", m.compare_one_sql(suffix, models[0][0]))):
            out[name] = {"sql": sql, "analyze": client("ANALYZE " + sql)}
        json.dump(out, open(dump, "w"), indent=1)
    m.print_plans = wrap
    sys.argv = ["scenario1_backtest.py"] + args
    m.main()
