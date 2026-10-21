#pragma once

// BS-S7's history checker (instructions/v3.0.0/workorder-bs-test-hardening.md):
// Elle's list-append inference, over a history this file's cells record.
//
// Every write appends a value unique to its key to a list; every read
// returns a key's whole list. The final read of every key fixes each key's
// version order, and from it the dependency graph among committed
// transactions follows (Adya's, as Elle infers it):
//
//   ww  T1 -> T2   T2 appended the element right after T1's on a key
//   wr  T1 -> T2   T2 read a list whose last element T1 appended
//   rw  T1 -> T2   T1 read a list, and T2 appended the element after its last
//
// and the anomalies `docs/spec/txn.md` §1 rules out are cycles and reads of
// one shape or another:
//
//   G1a      a read holds an element an aborted transaction appended
//   G1b      a read ends in an element its writer appended again after
//   order    a read is not a prefix of its key's final list, or a committed
//            append is missing from it (a lost write)
//   atomic   an indeterminate transaction's appends are in part present
//   monotonic a later read of a key in one transaction sees less of it than
//            an earlier one did: READ COMMITTED's "everything committed
//            before the statement began"
//   G0       a cycle of ww edges
//   G1c      a cycle of ww and wr edges
//   G-single a cycle with exactly one rw edge: read skew
//   G-nonadjacent a cycle with rw edges, no two of them adjacent (a long
//            fork among them)
//
// REPEATABLE READ is a snapshot at BEGIN with first-updater-wins (txn.md §1,
// §5) - snapshot isolation - whose admissible cycles are exactly those with
// two adjacent rw edges (Cerone and Gotsman), so G-single and G-nonadjacent
// are its, and READ COMMITTED allows both. G2 with adjacent rw edges - write
// skew - both levels allow (no SERIALIZABLE), so it is never reported. A
// transaction whose COMMIT answered an error, or that a crash cut off, is
// indeterminate, and the final lists decide it: every append present is a
// commit, none present an abort.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kds::history {

enum class Outcome : std::uint8_t { kCommitted, kAborted, kIndeterminate };

struct Op {
    bool append = false;
    std::uint64_t key = 0;
    std::uint64_t value = 0;            // an append's element
    std::vector<std::uint64_t> read;    // a read's list, in order
};

struct Txn {
    std::uint64_t id = 0;
    Outcome outcome = Outcome::kAborted;
    std::vector<Op> ops;  // the ones that answered; an errored op ends the txn
};

struct Anomaly {
    std::string kind;
    std::string detail;
};

struct Result {
    std::vector<Anomaly> anomalies;
    std::size_t committed = 0;
    std::size_t edges = 0;
    std::size_t rw_edges = 0;
};

// `final_lists`: each key's list as one read after every transaction ended.
// `forbid_g_single`: REPEATABLE READ - G-single and G-nonadjacent.
inline Result Check(const std::vector<Txn>& txns,
                    const std::map<std::uint64_t, std::vector<std::uint64_t>>& final_lists,
                    bool forbid_g_single) {
    Result r;
    const auto add = [&r](std::string kind, std::string detail) {
        if (r.anomalies.size() < 32) r.anomalies.push_back({std::move(kind), std::move(detail)});
    };
    // Who appended each (key, element), and where in its transaction.
    std::map<std::pair<std::uint64_t, std::uint64_t>, const Txn*> writer;
    std::map<std::pair<std::uint64_t, std::uint64_t>, bool> writers_last;  // its last append there
    for (const Txn& t : txns) {
        if (t.outcome == Outcome::kCommitted) ++r.committed;
        std::map<std::uint64_t, std::uint64_t> last;
        for (const Op& op : t.ops) {
            if (!op.append) continue;
            writer[{op.key, op.value}] = &t;
            last[op.key] = op.value;
        }
        for (const Op& op : t.ops) {
            if (op.append) writers_last[{op.key, op.value}] = last[op.key] == op.value;
        }
    }
    const auto name = [](const Txn* t) { return "T" + std::to_string(t->id); };

    // Final lists: every element appended once, by somebody.
    std::set<std::pair<std::uint64_t, std::uint64_t>> present;
    for (const auto& [key, list] : final_lists) {
        for (const std::uint64_t element : list) {
            if (writer.count({key, element}) == 0) {
                add("order", "key " + std::to_string(key) + " holds " + std::to_string(element) +
                                 ", which no transaction appended");
            }
            if (!present.insert({key, element}).second) {
                add("order", "key " + std::to_string(key) + " holds " + std::to_string(element) +
                                 " twice");
            }
        }
    }
    // An indeterminate transaction is what its appends say it was; a
    // read-only one stays indeterminate, its reads checked as a commit's.
    std::map<const Txn*, Outcome> outcome;
    for (const Txn& t : txns) {
        Outcome o = t.outcome;
        if (o == Outcome::kIndeterminate) {
            std::size_t appends = 0, seen = 0;
            for (const Op& op : t.ops) {
                if (!op.append) continue;
                ++appends;
                seen += present.count({op.key, op.value});
            }
            if (appends > 0 && seen == appends) o = Outcome::kCommitted;
            if (appends > 0 && seen == 0) o = Outcome::kAborted;
            if (seen > 0 && seen < appends) {
                add("atomic", "indeterminate " + name(&t) + " has " + std::to_string(seen) +
                                  " of its " + std::to_string(appends) + " appends present");
            }
        }
        outcome[&t] = o;
    }
    const auto aborted = [&outcome](const Txn* t) { return outcome.at(t) == Outcome::kAborted; };
    // No aborted element in a final list, every committed one.
    for (const auto& [kv, t] : writer) {
        const bool there = present.count(kv) != 0;
        if (there && aborted(t)) {
            add("G1a", "key " + std::to_string(kv.first) + "'s final list holds " +
                           std::to_string(kv.second) + " of aborted " + name(t));
        }
        if (!there && t->outcome == Outcome::kCommitted) {
            add("order", "committed " + name(t) + "'s append of " + std::to_string(kv.second) +
                             " to key " + std::to_string(kv.first) + " was lost");
        }
    }

    // The graph over every transaction not aborted.
    enum Edge : std::uint8_t { kWw = 1, kWr = 2, kRw = 4 };
    std::map<const Txn*, std::map<const Txn*, std::uint8_t>> g;
    const auto link = [&](const Txn* a, const Txn* b, Edge e) {
        if (a == b) return;
        g[a][b] |= e;
    };
    for (const auto& [key, list] : final_lists) {
        for (std::size_t i = 1; i < list.size(); ++i) {
            const auto a = writer.find({key, list[i - 1]});
            const auto b = writer.find({key, list[i]});
            if (a != writer.end() && b != writer.end()) link(a->second, b->second, kWw);
        }
    }
    for (const Txn& t : txns) {
        if (aborted(&t)) continue;
        std::map<std::uint64_t, std::vector<std::uint64_t>> own;  // its appends so far, per key
        std::map<std::uint64_t, std::size_t> others;  // the most of others' it read, per key
        for (const Op& op : t.ops) {
            if (op.append) {
                own[op.key].push_back(op.value);
                continue;
            }
            // A transaction reads its own appends, last, in order.
            const std::size_t mine = own[op.key].size();
            if (op.read.size() < mine ||
                !std::equal(own[op.key].rbegin(), own[op.key].rend(), op.read.rbegin())) {
                add("internal", name(&t) + " read key " + std::to_string(op.key) +
                                    " without its own appends at the end");
                continue;
            }
            // An aborted transaction's element in what a committed one read.
            bool dirty = false;
            for (const std::uint64_t element : op.read) {
                const auto w = writer.find({op.key, element});
                if (w != writer.end() && w->second != &t && aborted(w->second)) {
                    add("G1a", name(&t) + " read " + std::to_string(element) + " of aborted " +
                                   name(w->second) + " on key " + std::to_string(op.key));
                    dirty = true;
                }
            }
            if (dirty) continue;
            const auto fin = final_lists.find(op.key);
            const std::vector<std::uint64_t> empty;
            const std::vector<std::uint64_t>& order = fin == final_lists.end() ? empty : fin->second;
            // A read is a prefix of the key's final list.
            if (op.read.size() > order.size() ||
                !std::equal(op.read.begin(), op.read.end(), order.begin())) {
                add("order", name(&t) + " read key " + std::to_string(op.key) +
                                 " as a list that is not a prefix of its final one");
                continue;
            }
            // A later statement sees at least what an earlier one did.
            std::size_t& most = others[op.key];
            if (op.read.size() - mine < most) {
                add("monotonic", name(&t) + " read key " + std::to_string(op.key) + " shorter (" +
                                     std::to_string(op.read.size() - mine) + ") than before (" +
                                     std::to_string(most) + ")");
            }
            most = std::max(most, op.read.size() - mine);
            if (!op.read.empty()) {
                const auto w = writer.find({op.key, op.read.back()});
                if (w != writer.end()) {
                    if (w->second != &t && !writers_last[{op.key, op.read.back()}]) {
                        add("G1b", name(&t) + " read key " + std::to_string(op.key) +
                                       " ending in an intermediate append of " + name(w->second));
                    }
                    link(w->second, &t, kWr);
                }
            }
            if (op.read.size() < order.size()) {
                const auto next = writer.find({op.key, order[op.read.size()]});
                if (next != writer.end()) link(&t, next->second, kRw);
            }
        }
    }
    for (const auto& [a, out] : g) {
        r.edges += out.size();
        for (const auto& [b, e] : out) r.rw_edges += (e & kRw) != 0;
    }

    // Reachability over the edges in `mask`, from `from`, looking for `to`.
    const auto reaches = [&](const Txn* from, const Txn* to, std::uint8_t mask) {
        std::set<const Txn*> seen{from};
        std::vector<const Txn*> stack{from};
        while (!stack.empty()) {
            const Txn* at = stack.back();
            stack.pop_back();
            const auto it = g.find(at);
            if (it == g.end()) continue;
            for (const auto& [b, e] : it->second) {
                if ((e & mask) == 0) continue;
                if (b == to) return true;
                if (seen.insert(b).second) stack.push_back(b);
            }
        }
        return false;
    };
    // From `from`, entered by an rw edge, back to `to`, whose own edge out is
    // rw: a path with no two rw edges in a row and none at either end.
    const auto reaches_nonadjacent = [&](const Txn* from, const Txn* to) {
        using At = std::pair<const Txn*, bool>;  // a node, and whether an rw edge entered it
        std::set<At> seen{{from, true}};
        std::vector<At> stack{{from, true}};
        while (!stack.empty()) {
            const auto [at, after_rw] = stack.back();
            stack.pop_back();
            const auto it = g.find(at);
            if (it == g.end()) continue;
            for (const auto& [b, e] : it->second) {
                if (e & (kWw | kWr)) {
                    if (b == to) return true;
                    if (seen.insert({b, false}).second) stack.push_back({b, false});
                }
                if ((e & kRw) && !after_rw && b != to && seen.insert({b, true}).second) {
                    stack.push_back({b, true});
                }
            }
        }
        return false;
    };
    // Each class: an edge a->b of it closes a cycle with b ~> a.
    for (const auto& [a, out] : g) {
        for (const auto& [b, e] : out) {
            if ((e & kWw) && reaches(b, a, kWw)) {
                add("G0", "ww cycle through " + name(a) + " and " + name(b));
            } else if ((e & (kWw | kWr)) && reaches(b, a, kWw | kWr)) {
                add("G1c", "ww/wr cycle through " + name(a) + " and " + name(b));
            } else if (forbid_g_single && (e & kRw) && reaches(b, a, kWw | kWr)) {
                add("G-single", "one rw edge " + name(a) + " -> " + name(b) +
                                    " closed by ww/wr edges back");
            } else if (forbid_g_single && (e & kRw) && reaches_nonadjacent(b, a)) {
                add("G-nonadjacent", "rw edge " + name(a) + " -> " + name(b) +
                                         " closed by a path with no two rw edges adjacent");
            }
        }
    }
    return r;
}

}  // namespace kds::history
