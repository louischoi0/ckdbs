#include "sim/minimize.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace kds::sim {

std::string FailureSignature(const SimVerdict& verdict) {
    if (verdict.ok) return {};
    // Everything run-specific goes: the bracketed SQL and fault trace, the
    // quoted row contents, and every number. What is left is the sentence
    // the harness would say about any run that failed this way.
    std::string out;
    bool in_bracket = false;
    bool in_quote = false;
    bool in_digits = false;
    for (const char c : verdict.detail) {
        if (c == '[') { in_bracket = true; continue; }
        if (c == ']') { in_bracket = false; continue; }
        if (in_bracket) continue;
        if (c == '\'') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        if (c >= '0' && c <= '9') {
            if (!in_digits) {
                out.push_back('#');
                in_digits = true;
            }
            continue;
        }
        in_digits = false;
        out.push_back(c);
    }
    return out;
}

std::string MinimizeOutcome::Summary() const {
    if (!found_failure) return "MINIMIZE: no iteration failed; nothing to shrink";
    std::ostringstream os;
    os << "MINIMIZE iteration=" << iteration << " ops " << ops_before << " -> " << ops_after
       << " in " << replays << " replay(s)\n  signature:" << signature;
    return os.str();
}

namespace {

// One shrink pass set: try to delete a contiguous chunk, keep the deletion
// when the same failure survives, and halve the chunk when a whole sweep
// finds nothing to drop. Plain delta debugging without the complement
// phase — the complement half buys little here, because an op list's
// failures are usually carried by a *prefix* of state-building ops plus one
// trigger, and dropping chunks finds that shape directly.
SimPlan Shrink(const SimConfig& config, SimPlan plan, std::size_t iteration,
               const std::string& target, std::size_t max_replays, std::size_t& replays) {
    std::size_t granularity = 2;
    while (plan.entries.size() > 1 && replays < max_replays) {
        const std::size_t chunk =
            (plan.entries.size() + granularity - 1) / granularity;
        bool progress = false;
        for (std::size_t start = 0; start < plan.entries.size(); start += chunk) {
            if (replays >= max_replays) break;
            SimPlan candidate = plan;
            const auto first = candidate.entries.begin() + static_cast<std::ptrdiff_t>(start);
            const auto last =
                first + static_cast<std::ptrdiff_t>(
                            std::min(chunk, static_cast<std::size_t>(
                                                candidate.entries.end() - first)));
            candidate.entries.erase(first, last);
            if (candidate.entries.empty()) continue;
            ++replays;
            if (FailureSignature(RunPlan(config, candidate, iteration)) != target) continue;
            plan = std::move(candidate);
            progress = true;
            break;  // restart the sweep at this granularity over the smaller plan
        }
        if (progress) continue;
        if (granularity >= plan.entries.size()) break;
        granularity = std::min(granularity * 2, plan.entries.size());
    }
    return plan;
}

}  // namespace

MinimizeOutcome MinimizeFailure(const SimConfig& config, std::size_t max_replays) {
    MinimizeOutcome out;
    for (std::size_t i = 0; i < config.iterations; ++i) {
        SimPlan plan = BuildPlan(config, i);
        const SimVerdict verdict = RunPlan(config, plan, i);
        if (verdict.ok) continue;

        out.found_failure = true;
        out.iteration = i;
        out.signature = FailureSignature(verdict);
        out.ops_before = plan.entries.size();
        out.plan = Shrink(config, std::move(plan), i, out.signature, max_replays, out.replays);
        out.ops_after = out.plan.entries.size();
        return out;
    }
    return out;
}

// ---- The `.sim` case file -------------------------------------------------
//
// Tab-separated, `sql` last: a generated value is [a-z] only and a table
// name is [a-z0-9], so no field can contain a tab and the format needs no
// escaping. Fault lines precede the op they are armed before.

namespace {

std::string TogglesField(const FeatureToggles& toggles) {
    std::string out;
    out += toggles.waystone ? '1' : '0';
    out += toggles.cabins ? '1' : '0';
    out += toggles.access_statistics ? '1' : '0';
    return out;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t at = 0;
    while (true) {
        const std::size_t next = line.find('\t', at);
        if (next == std::string::npos) {
            fields.push_back(line.substr(at));
            return fields;
        }
        fields.push_back(line.substr(at, next - at));
        at = next + 1;
    }
}

// Whether `field` is `key<value>`, and its value if so. The presence of
// the key, not the emptiness of the value, is what selects a branch: a
// `key=` with nothing after it is a malformed field to be reported, never
// a field that silently belongs to the next key in the chain.
bool ParseUnsigned(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = value;
    return true;
}

bool ParseSigned(const std::string& text, std::int64_t& out) {
    const bool negative = !text.empty() && text[0] == '-';
    std::uint64_t magnitude = 0;
    if (!ParseUnsigned(negative ? text.substr(1) : text, magnitude)) return false;
    out = negative ? -static_cast<std::int64_t>(magnitude)
                   : static_cast<std::int64_t>(magnitude);
    return true;
}

bool Field(const std::string& field, std::string_view key, std::string& value) {
    if (field.size() < key.size() || field.compare(0, key.size(), key) != 0) return false;
    value = field.substr(key.size());
    return true;
}


}  // namespace

Status WriteCase(const std::string& path, const SimConfig& config, const SimPlan& plan,
                 const std::string& signature) {
    std::ofstream out(path);
    if (!out) return Status::IoError("cannot write the case file " + path);

    out << "# ckdbs-sim case (bench/workplan-teststrategy SIM07)\n";
    out << "# failure:" << signature << "\n";
    out << "config\tseed=" << config.seed << "\tmode=" << SimModeName(config.mode)
        << "\tprofile=" << ProfileName(config.profile)
        << "\tfaults=" << FaultProfileName(config.faults)
        << "\ttoggles=" << TogglesField(plan.toggles)
        // The two run-level gates. Without them a case minimized under
        // `--skip-recovery` — which is the harness's own planted bug, and
        // the one the minimizer is demonstrated on — replays green, so the
        // primary artifact of SIM07 would be a file that says it fails and
        // does not.
        << "\tskip-recovery=" << (config.skip_recovery ? 1 : 0)
        << "\tassert-recovery=" << (config.assert_recovery ? 1 : 0) << "\n";

    for (const SimPlan::Entry& entry : plan.entries) {
        for (const FaultKind kind : entry.faults) {
            out << "fault\t" << FaultKindName(kind) << "\n";
        }
        const Op& op = entry.op;
        out << "op\t" << OpKindName(op.kind) << '\t' << op.table << '\t' << op.key << '\t'
            << op.lo << '\t' << op.hi << '\t' << op.v << '\t' << op.pred_v << '\t'
            << (op.btree ? 1 : 0) << '\t' << (op.by_pk ? 1 : 0) << '\t'
            << (op.set_name ? 1 : 0) << '\t' << op.name << '\t' << op.sql << "\n";
    }
    if (!out) return Status::IoError("the case file " + path + " was not written whole");
    return Status::OK();
}

StatusOr<LoadedCase> ReadCase(const std::string& path) {
    std::ifstream in(path);
    if (!in) return Status::IoError("cannot read the case file " + path);

    LoadedCase loaded;
    std::vector<FaultKind> pending;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = SplitTabs(line);
        const std::string& tag = f[0];

        if (tag == "config") {
            for (std::size_t i = 1; i < f.size(); ++i) {
                std::string v;
                if (Field(f[i], "seed=", v)) {
                    if (!ParseUnsigned(v, loaded.config.seed)) {
                        return Status::InvalidArgument("case file: seed is not a number");
                    }
                } else if (Field(f[i], "mode=", v)) {
                    const auto mode = ParseSimMode(v);
                    if (!mode.has_value()) {
                        return Status::InvalidArgument("case file: unknown mode '" + v + "'");
                    }
                    loaded.config.mode = *mode;
                } else if (Field(f[i], "profile=", v)) {
                    const auto profile = ParseProfile(v);
                    if (!profile.has_value()) {
                        return Status::InvalidArgument("case file: unknown profile '" + v + "'");
                    }
                    loaded.config.profile = *profile;
                } else if (Field(f[i], "faults=", v)) {
                    const auto faults = ParseFaultProfile(v);
                    if (!faults.has_value()) {
                        return Status::InvalidArgument("case file: unknown fault profile '" + v +
                                                       "'");
                    }
                    loaded.config.faults = *faults;
                } else if (Field(f[i], "skip-recovery=", v)) {
                    loaded.config.skip_recovery = v == "1";
                } else if (Field(f[i], "assert-recovery=", v)) {
                    loaded.config.assert_recovery = v == "1";
                } else if (Field(f[i], "toggles=", v)) {
                    if (v.size() != 3) {
                        return Status::InvalidArgument("case file: toggles is not three digits");
                    }
                    loaded.plan.toggles.waystone = v[0] == '1';
                    loaded.plan.toggles.cabins = v[1] == '1';
                    loaded.plan.toggles.access_statistics = v[2] == '1';
                } else {
                    return Status::InvalidArgument("case file: unknown config field '" + f[i] +
                                                   "'");
                }
            }
            // The plan's own toggles are authoritative; pinning them here
            // keeps a replay from re-drawing them off the seed.
            loaded.config.toggles = loaded.plan.toggles;
            loaded.config.iterations = 1;
            continue;
        }

        if (tag == "fault") {
            if (f.size() < 2) return Status::InvalidArgument("case file: a fault line has no kind");
            const std::optional<FaultKind> kind = ParseFaultKind(f[1]);
            if (!kind.has_value()) {
                return Status::InvalidArgument("case file: unknown fault kind '" + f[1] + "'");
            }
            pending.push_back(*kind);
            continue;
        }

        if (tag != "op") {
            return Status::InvalidArgument("case file line " + std::to_string(line_no) +
                                           ": unknown tag '" + tag + "'");
        }
        constexpr std::size_t kOpFields = 13;
        if (f.size() < kOpFields) {
            return Status::InvalidArgument("case file line " + std::to_string(line_no) +
                                           ": an op line has " + std::to_string(f.size()) +
                                           " fields, not " + std::to_string(kOpFields));
        }
        const std::optional<Op::Kind> kind = ParseOpKind(f[1]);
        if (!kind.has_value()) {
            return Status::InvalidArgument("case file: unknown op kind '" + f[1] + "'");
        }
        Op op;
        op.kind = *kind;
        op.table = f[2];
        // Every number through a checked parse: this file is hand-edited by
        // the person triaging a case, and `std::stoull` on a typo
        // terminates the process instead of naming the line.
        if (!ParseUnsigned(f[3], op.key) || !ParseUnsigned(f[4], op.lo) ||
            !ParseUnsigned(f[5], op.hi) || !ParseSigned(f[6], op.v) ||
            !ParseSigned(f[7], op.pred_v)) {
            return Status::InvalidArgument("case file line " + std::to_string(line_no) +
                                           ": a numeric field is not a number");
        }
        op.btree = f[8] == "1";
        op.by_pk = f[9] == "1";
        op.set_name = f[10] == "1";
        op.name = f[11];
        op.sql = f[12];
        loaded.plan.entries.push_back(SimPlan::Entry{std::move(op), std::move(pending)});
        pending.clear();
    }
    if (!pending.empty()) {
        return Status::InvalidArgument("case file " + path +
                                       " ends with a fault line that no op follows");
    }
    if (loaded.plan.entries.empty()) {
        return Status::InvalidArgument("case file " + path + " holds no ops");
    }
    return loaded;
}

}  // namespace kds::sim
