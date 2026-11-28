#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"

// A fold that reads a row where it lies (EH-S1,
// `instructions/v3.2.0/workorder-eh-native-fold-per-row.md`).
//
// A statement's rows reach its fold through the row sink: the visible row's
// payload is copied out from under the leaf's hold, decoded into the frame,
// passed through the step's conjuncts and handed over (`step_vm.cpp`,
// `AcceptTupleAt`). For a fold over one relation with no predicate every part
// of that but the visibility test is spent on a row nothing will look at
// again, and EH-S0 priced it: a counted row cost 19.2 ns of server CPU where a
// row the scan kernel rejected cost 10.9 ns.
//
// This is the other path for that fold: the executor hands the **visible
// version's bytes, under the hold that makes them readable**, to a consumer
// that takes what it needs from them and keeps no pointer into them.
//
// What the executor guarantees a consumer (`step_vm.cpp` states where):
//   - `payload` is the version `txn::Classify` answered `kVisible` for under
//     the statement's snapshot, read under the page's span (invariant 10);
//     a version that needs the undo log never comes here - it takes the
//     copied path and the row sink, as before;
//   - `payload.size()` is the layout's row size (invariant 13 is checked
//     first; a payload of another length takes the copied path, whose decode
//     refuses it);
//   - the step's shape checks have run for this statement (BM-R6), so no
//     offset is read before the layout was checked;
//   - the row is counted and charged exactly as a row the residual keeps.
//
// What a consumer owes: no page fetch, no latch and no wait in
// `AccumulateBytes` (it runs under a leaf's read hold); nothing of `payload`
// kept past the call; and the same answer, refusal and refusal order the row
// sink would have produced for the same row (`EH-R1`, `EH-R4`). A fold that
// read a version its snapshot does not see, or kept bytes past the hold, would
// answer wrongly with nothing to show for it (`EH-R2`, `EH-R3`,
// `[quiet-wrong]`).

namespace kds::exec {

class BytesFold {
public:
    virtual ~BytesFold() = default;

    // Asked once a statement, at the first visible row of the chain's one
    // step, with the relation that step is bound to. Answers whether every
    // row of it can be folded from its payload alone; false leaves every row
    // on the row sink.
    virtual bool PrepareBytesFold(const catalog::Schema& schema, const catalog::RowLayout& layout,
                                  std::uint16_t rel_slot) = 0;

    // One visible row, under its leaf's hold.
    virtual Status AccumulateBytes(std::span<const std::byte> payload) = 0;
};

}  // namespace kds::exec
