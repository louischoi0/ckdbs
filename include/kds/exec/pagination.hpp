#pragma once

#include <cstdint>
#include <optional>

#include "kds/exec/step_chain.hpp"

// The emission quota - V09's execution half (docs/spec/parser-v2.md I11).
//
// ---- Why this is a sink decorator and not a step -------------------------
//
// AG1's seam, for AG1's reason: the quota consumes rows and has no opinion
// about where they came from. It wraps the dispatcher's RowSink, so the
// compiled chain of a limited statement is its unlimited twin's - steps,
// kinds, keys, residuals, bit for bit - and every property already proved
// of a chain (trail trust, probe equivalence, access statistics, "passing
// a replay cannot change a reply") holds for a limited statement with no
// new proof. The step VM is untouched by construction: a change to
// step_vm.cpp in service of pagination means this placement was violated.
//
// What makes the decorator *cheap* rather than merely correct is V03: a
// RowSink already answers `VisitControl`, and `kStop` ends the statement
// successfully - the walk stops on the very tuple that filled the quota
// and fetches no further page. Early termination is the existing stop
// propagation, not new machinery.
//
// **That cheapness argument is the chain caller's alone.** A catalog view
// (`SELECT ... FROM sys.tables`) has no walk to stop: the dispatcher
// answers it from `exec::ReadCatalogView`, which materializes every row
// of the view before this object exists. The quota is exactly as
// *correct* there - same verdicts, same [m, m+n) reply - and buys only
// the rendering of the rows it skips, never the reading of them. So
// `LIMIT 1` over a view costs a whole view, and a client-facing promise
// that an offset costs O(offset) is a statement about relations, not
// about `sys.*`.
//
// ---- The contract --------------------------------------------------------
//
// Emission order is a client contract (I12: written order across steps,
// pk order within one), so the reply to `LIMIT n OFFSET m` is exactly
// rows [m, m+n) of the reply the unlimited statement gives - a prefix of
// an order the statement has, not one it hopes for. OFFSET skips
// *qualifying* rows: they are examined, filtered and then discarded, so
// an offset costs what it skips (no head seek exists - the same note
// `kRange` pruning carries), and skipped rows still charge the row-touch
// budget, deliberately: the quota bounds output, the budget bounds work.
//
// One quota per statement, applied at the top-level emission boundary
// only. Sub-chains build their own sinks inside the VM and are untouched
// - a subquery's rows feed a predicate, not the client - and the parser
// refuses a tail anywhere but the outermost non-aggregated block, so
// nothing else can carry one.

namespace kds::exec {

// What the quota says about one qualifying row, decided *before* the row
// is formatted so a skipped row costs no rendering.
enum class QuotaVerdict : std::uint8_t {
    kSkip,          // an OFFSET row: examined, not emitted, keep walking
    kEmit,          // emit and keep walking
    kEmitThenStop,  // emit; the quota is now full - stop the statement
    kStop,          // LIMIT 0: emit nothing, stop before the first row
};

class EmissionQuota {
public:
    explicit EmissionQuota(const StepChain& chain) noexcept
        : EmissionQuota(chain.offset, chain.limit) {}

    // The tail on its own, for the one row source that is not a chain: a
    // catalog view is materialized by the catalog's typed readers, so it
    // has no `StepChain` to read the clause off. The quota is the same
    // object either way, which is the point - one place decides what
    // `LIMIT n OFFSET m` means, and a view's reply is rows [m, m+n) of
    // its unlimited reply by the same rule every other statement obeys.
    //
    // `explicit`, and offset first: SQL writes `LIMIT n OFFSET m`, so a
    // reader's eye supplies the opposite order to the one the arguments
    // take. The keyword blocks the brace-initialized call - `f({0, 5})` -
    // which is the form that would swap them invisibly; a direct call
    // still has to name what it passes.
    explicit EmissionQuota(std::uint64_t offset, std::optional<std::uint64_t> limit) noexcept
        : offset_(offset), limit_(limit) {}

    // Called once per row the chain produced, in emission order. Not
    // idempotent - each call advances the quota - so exactly one call per
    // row, before formatting it.
    QuotaVerdict Note() noexcept {
        if (limit_.has_value() && limit_.value() == 0) return QuotaVerdict::kStop;
        if (skipped_ < offset_) {
            ++skipped_;
            return QuotaVerdict::kSkip;
        }
        ++emitted_;
        if (limit_.has_value() && emitted_ >= limit_.value()) return QuotaVerdict::kEmitThenStop;
        return QuotaVerdict::kEmit;
    }

    std::uint64_t emitted() const noexcept { return emitted_; }

private:
    std::uint64_t offset_;
    std::optional<std::uint64_t> limit_;
    std::uint64_t skipped_ = 0;
    std::uint64_t emitted_ = 0;
};

}  // namespace kds::exec
