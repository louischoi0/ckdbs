#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/captured_plans.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::wal {
class WalManager;
}

// **`sys.plans`: the plans a statement shape is held to, as catalog rows**
// (CX-S1a, instructions/v3.2.0/workorder-cx-managed-plans.md CX-R8;
// docs/spec/parser-v2.md I12). At this stage every row is a **captured**
// plan: the join order `join_order = fixed` fixed for a shape
// (`captured_plans.hpp`), written here so it outlives the run.
//
// **Who writes, and when.** Never a statement: a `SELECT` writes no log
// (CF's rule), and a `READ ONLY` transaction no record. The compile installs
// a capture in memory and queues it; core 0's `system` task calls
// `PersistCapturedPlans` once a second and a clean shutdown calls it once
// more, each row a logged insert at `kNoTxnId`, as every declaration
// relation's is (`declaration_relation.hpp`). A capture a crash finds still
// queued is lost; its shape is captured again at its next compile, possibly
// to another order (CX-Q14 (a)).
//
// **A row is the capture, whole**: the shape's key - the fetch id in the
// relation's `aux` column, every relation oid in written order in the text -
// the order, when it was written, and the capturing statement's shape text
// for a reader. A mount (`LoadCapturedPlans`) installs each row's order in
// the instance's table before any statement compiles, so a captured shape is
// never searched again after a restart, a crash or a restore.
//
// **No schema word moves** (CX-Q23 (a)): a row names no relation's schema
// and no compile reads the relation, only the table a mount filled from it.
//
// **A dropped relation's rows** (CX-Q24): an oid is never reissued, so a row
// naming a dropped relation can match nothing. `CapturedPlans::
// ForgetRelation`, called where a `DROP TABLE` commits, owes their removal
// to the persisting task; a mount removes any a crash left. Not in the
// drop's own transaction, as CX-Q24 (a) had it: a declaration relation's
// delete is physical and outside every transaction, and the rows are inert
// from the commit on.
//
// The row's text is `v1 fp=<v> oids=<o>,<o> order=<p>,<p> at=<us>` and,
// after one newline, the statement. `fp` is the fingerprint version the
// fetch id was taken under (`parser::kFingerprintVersion`): a row of another
// version can match no statement of this build, and a mount removes it.
// `order` is written positions in executed order. A row this build cannot
// read is skipped by a mount and shown as it is by `sys.plans`; it is never
// guessed at.
//
// **One row a shape.** A mount that meets a second row for a key it has
// already loaded removes it, so which row a later mount meets first never
// decides an order.
//
// **Cost.** A step's inserts skip the declaration relations' name check -
// a scan of the whole relation a row - because a captured row's name holds
// its own id, and its removals are one walk for the step
// (`DeleteDeclarationsById`), so a step is linear in what it writes, not in
// what the relation holds.

namespace kds::exec {

// `DeclarationRow::flags` bit 0: the row is a captured plan.
inline constexpr std::uint32_t kPlanFlagCaptured = 1u << 0;

struct PlanRow {
    std::uint64_t id = 0;
    std::string name;
    bool captured = false;
    // False for a row whose text this build cannot read; `key`, `order` and
    // `written_at_us` are then unset and `statement` is the row's text.
    bool readable = false;
    std::uint32_t fingerprint_version = 0;
    CapturedKey key;
    CapturedOrder order{};
    std::int64_t written_at_us = 0;  // UTC epoch microseconds
    std::string statement;
};

// The text a captured plan's row stores, and its reading. `Corruption`
// where the text is not this format, names a count over the key's width,
// or its order is not a permutation of its relations' positions; a refused
// text leaves `out` as it was.
std::string EncodePlanText(const CapturedPlan& plan, std::int64_t written_at_us);
Status DecodePlanText(std::string_view text, PlanRow& out);

// Every row of `sys.plans`, in chain order.
StatusOr<std::vector<PlanRow>> ListPlans(catalog::Catalog& catalog, storage::PageStore& store);

// One step of the persisting task: up to `max_rows` queued captures written
// as rows, then the rows naming a relation dropped since the last step
// removed. A capture a failed write could not place goes back on the queue
// and the step answers the failure; the next step tries again.
inline constexpr std::size_t kPlanPersistBatch = 64;
Status PersistCapturedPlans(catalog::Catalog& catalog, storage::PageStore& store,
                            wal::WalManager* wal, CapturedPlans& table, std::int64_t now_us,
                            std::size_t max_rows = kPlanPersistBatch);

// A mount's: every readable captured row installed in `table`, and removed
// every row that names a relation that no longer exists, was keyed under
// another fingerprint version, or repeats a key already loaded. Answers how
// many it could not read, which stay where they are.
StatusOr<std::size_t> LoadCapturedPlans(catalog::Catalog& catalog, storage::PageStore& store,
                                        wal::WalManager* wal, CapturedPlans& table);

// `DROP PLAN <name>`: removes the row and, for a captured plan, forgets its
// shape, so the shape's next compile under `fixed` captures again (CX-Q17).
// `NotFound` if no row has the name.
StatusOr<PlanRow> DropPlan(catalog::Catalog& catalog, storage::PageStore& store,
                           wal::WalManager* wal, CapturedPlans& table, std::string_view name);

}  // namespace kds::exec
