#!/usr/bin/env python3
"""Knob exports of ec5ddfe8: copies /home/louis/dfclose/srcec5ddfe8 to srcK<name>, applies the knob, writes K<name>.diff.
usage: mk.py K1 K2 K3 KA"""
import os, re, shutil, subprocess, sys

ROOT = "/home/louis/dfclose"
BASE = f"{ROOT}/srcec5ddfe8"


def sub1(path, old, new, count=1):
    s = open(path).read()
    assert s.count(old) == count, (path, old, s.count(old))
    open(path, "w").write(s.replace(old, new))


def k1(d):
    sub1(f"{d}/src/server/command_dispatcher.cpp", "    RecordIndexUse(chain, stats);\n",
         "    // KNOB K1: RecordIndexUse(chain, stats);\n")


def k2(d):
    p = f"{d}/include/kds/server/session.hpp"
    sub1(p, """        txn_began_ns_ = ns;
        if (wait_stats_ != nullptr) {
            wait_stats_->txn_began_ns.set(ns);
            wait_stats_->view_minted_ns.set(ns);
        }
""", "        txn_began_ns_ = ns;  // KNOB K2: the wait-statistics words are not set\n")
    sub1(p, """    void NoteViewMinted(std::uint64_t ns) noexcept {
        if (wait_stats_ != nullptr) wait_stats_->view_minted_ns.set(ns);
    }""", """    void NoteViewMinted(std::uint64_t) noexcept {}  // KNOB K2""")
    sub1(p, """    void EndStatementView() noexcept {
        if (state_ == State::kIdle && wait_stats_ != nullptr) wait_stats_->view_minted_ns.set(0);
    }""", """    void EndStatementView() noexcept {}  // KNOB K2""")


def k3(d):
    sub1(f"{d}/src/txn/instance_visibility.cpp",
         "    slots_[core].min_snapshot_minted_ns.store(minted_ns, std::memory_order_relaxed);\n",
         "    (void)minted_ns;  // KNOB K3: the stamp is not published\n")
    p = f"{d}/src/txn/manager.cpp"
    sub1(p, "    txn->view_minted_ns_ = now_ns;\n", "    (void)now_ns;  // KNOB K3\n")
    sub1(p, "    txn.view_minted_ns_ = now_ns;\n", "    (void)now_ns;  // KNOB K3\n")
    sub1(p, "        reader_minted_ns_[slot] = now_ns;\n", "        (void)now_ns;  // KNOB K3\n")
    sub1(p, "    reader_minted_ns_[lease.slot_] = now_ns;\n", "    (void)now_ns;  // KNOB K3\n")


KN = {"K1": [k1], "K2": [k2], "K3": [k3], "KA": [k1, k2, k3]}
for name in sys.argv[1:]:
    d = f"{ROOT}/src{name}"
    if os.path.exists(d):
        shutil.rmtree(d)
    shutil.copytree(BASE, d)
    for fn in KN[name]:
        fn(d)
    r = subprocess.run(["diff", "-ru", "--label", "a", "--label", "b"] + [], capture_output=True)
    out = subprocess.run(f"cd {ROOT} && diff -ru srcec5ddfe8/src src{name}/src; diff -ru srcec5ddfe8/include src{name}/include",
                         shell=True, capture_output=True, text=True).stdout
    open(f"{ROOT}/{name}.diff", "w").write(out)
    print(name, "patched", len(out.splitlines()), "diff lines", flush=True)
