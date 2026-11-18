#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/admin_plans.hpp"
#include "kds/exec/captured_plans.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::wal {
class WalManager;
}

// **`sys.plans`: the plans a statement shape is held to, as catalog rows**
// (CX-S1a, instructions/v3.2.0/workorder-cx-managed-plans.md CX-R8;
// docs/spec/parser-v2.md I12). A row is one of two origins: a **captured**
// plan, the join order `join_order = fixed` fixed for a shape
// (`captured_plans.hpp`), written here so it outlives the run; or an
// **administrator's** plan, made by `CREATE PLAN` (CX-S1b,
// `admin_plans.hpp`; the second half of this comment). One shape holds at
// most one row of each origin, and the administrator's outranks the
// captured one, which stays beneath it (CX-R9).
//
// **Who writes a captured row, and when.** Never a statement: a `SELECT` writes no log
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
// for a reader. A mount (`LoadPlans`) installs each row's order in
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
//
// ---- An administrator's row (CX-S1b) --------------------------------------
//
// Written by the plan DDL, at its statement, under the instance's DDL claim
// (`AdminPlans::TryBeginDdl`): logged at `kNoTxnId` as every declaration
// is, autocommit only. Its text is
//
//   v1 fp=<v> oids=<o>,<o> bindings=<b>,<b> args=<n> at=<us> variants=<k>
//   <name> <p>,<p> [access=<w>:<pin>;...] ELSE | WHEN <rule>   (k lines)
//   <the statement's shape text>
//
// A variant's `access=` field is there only where it pins (CX-S3a): one
// `<written position>:<pin>` a pinned relation, a pin being `TUPLE_KEY`,
// `INDEX`, `INDEX:<name>`, `CABIN` or `SCAN` - the DDL's words
// (`ArmName`) - with `!` before a forbidden one. A row with none reads as it did before pins existed.
//
// `bindings` are the statement's relations as it names them, in written
// order - what an `ORDER (...)` list is resolved against, so `ALTER PLAN
// ... REPLACE VARIANT` needs no statement to parse - and `args` its value
// positions, the bound on `ARG(k)`. No row holds a client's value: the
// statement is kept as its shape text and a rule holds only what the
// administrator wrote.
//
// **`flags` bit 1 is `DISABLED`.** A disabled plan stays a row and is
// served to no statement; it does not RESTRICT a `DROP TABLE`.
//
// **A change is a new row, then the old row's removal** (`ReplaceAdminPlan`):
// a declaration relation has no update and its delete is physical, so the
// new row is inserted first and a crash between the two leaves two rows of
// one name. A mount keeps the one with the higher id and removes the other.
//
// **`invalid` is derived, never stored** (CX-Q4 (b), CX-Q16): a plan whose
// relation is gone or whose fetch id was taken under another fingerprint
// version can match no statement of this build. The engine does not delete
// an administrator's object: the row stays, is not loaded, and reads
// `invalid` until `DROP PLAN`.
//
// **A captured row's name begins `captured_`**, and no administrator's plan
// may: a captured row's insert skips the name check on the strength of it.

namespace kds::exec {

// `DeclarationRow::flags` bit 0: the row is a captured plan.
inline constexpr std::uint32_t kPlanFlagCaptured = 1u << 0;
// Bit 1: an administrator's plan that is disabled.
inline constexpr std::uint32_t kPlanFlagDisabled = 1u << 1;

// The prefix of every captured row's name, reserved to them.
inline constexpr std::string_view kCapturedPlanPrefix = "captured_";

struct PlanRow {
    std::uint64_t id = 0;
    std::string name;
    bool captured = false;
    // False for a row whose text this build cannot read; every field below
    // but `statement` is then unset, and `statement` is the row's text.
    bool readable = false;
    std::uint32_t fingerprint_version = 0;
    CapturedKey key;
    // A captured plan's order; unset for an administrator's, whose orders
    // are its variants'.
    CapturedOrder order{};
    std::int64_t written_at_us = 0;  // UTC epoch microseconds
    std::string statement;
    // ---- An administrator's plan's alone ----------------------------------
    bool enabled = true;
    std::vector<std::string> bindings;  // written order
    std::uint32_t arguments = 0;        // the statement's value positions
    std::vector<PlanVariant> variants;  // rule order, the last `ELSE`
};

// The text a captured plan's row stores, and its reading. `Corruption`
// where the text is not this format, names a count over the key's width,
// or its order is not a permutation of its relations' positions; a refused
// text leaves `out` as it was.
std::string EncodePlanText(const CapturedPlan& plan, std::int64_t written_at_us);
Status DecodePlanText(std::string_view text, PlanRow& out);

// The same pair for an administrator's row, from and into `row`'s key,
// bindings, argument count, variants and statement. The decode re-parses
// each rule (`ParsePlanRule`) and checks what the DDL checked: one binding
// a relation, each order a permutation, `ELSE` last and only last, no rule
// reading past the argument count.
std::string EncodeAdminPlanText(const PlanRow& row, std::int64_t written_at_us);
Status DecodeAdminPlanText(std::string_view text, PlanRow& out);

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

// A mount's. Every readable captured row is installed in `captured`, and
// removed where it names a relation that no longer exists, was keyed under
// another fingerprint version, or repeats a key already loaded. Every
// administrator's row whose relations exist is installed in `admin`, served
// if it is enabled; the older of two rows of one name is removed (a change
// a crash cut short), and an invalid row is left where it is. Answers how
// many rows it could not read, which stay too.
StatusOr<std::size_t> LoadPlans(catalog::Catalog& catalog, storage::PageStore& store,
                                wal::WalManager* wal, CapturedPlans& captured,
                                AdminPlans& admin);

// The row named `name`, or `NotFound`.
StatusOr<PlanRow> FindPlan(catalog::Catalog& catalog, storage::PageStore& store,
                           std::string_view name);

// Whether an administrator's row can match a statement of this build: its
// fingerprint version is this build's and no relation it names is gone.
// Only `NotFound` says a relation is gone; a relation that could not be
// opened for another reason is taken to exist, so a fault at a mount does
// not take a plan - and its `DROP TABLE` RESTRICT - out of force.
bool PlanRowIsValid(catalog::Catalog& catalog, const PlanRow& row);

// `CREATE PLAN`'s write: `row` - an administrator's, its `id` unset - as a
// new row under a checked name, installed in `admin` and served. The caller
// holds the DDL claim and has asked `AdminPlans::Holder` of the shape.
// `AlreadyExists` where the name is taken.
StatusOr<PlanRow> CreateAdminPlan(catalog::Catalog& catalog, storage::PageStore& store,
                                  wal::WalManager* wal, AdminPlans& admin, PlanRow row,
                                  std::int64_t now_us);

// `ALTER PLAN`'s write: `next` in place of the row `old_id`, under the same
// name - a new row first, then the old one's removal - and installed in
// `admin`, served if `next.enabled`. The caller holds the DDL claim.
StatusOr<PlanRow> ReplaceAdminPlan(catalog::Catalog& catalog, storage::PageStore& store,
                                   wal::WalManager* wal, AdminPlans& admin,
                                   std::uint64_t old_id, PlanRow next, std::int64_t now_us);

// `DROP PLAN <name>`: removes the row. A captured plan's shape is forgotten,
// so its next compile under `fixed` captures again (CX-Q17); an
// administrator's plan leaves its shape to the captured row beneath it, or
// to the search. `NotFound` if no row has the name. The caller holds the
// DDL claim.
StatusOr<PlanRow> DropPlan(catalog::Catalog& catalog, storage::PageStore& store,
                           wal::WalManager* wal, CapturedPlans& captured, AdminPlans& admin,
                           std::string_view name);

}  // namespace kds::exec
