#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "kds/exec/step_compiler.hpp"
#include "kds/txn/lock_table.hpp"

// ---- The read borrow (AO-S6e-b; AO-R12, AR2 §3's `SELECT` row; moved to
// the bind at AT-S1, `workorder-at-m3-uniformity.md` AT-R1; the relation
// alone since CP-S2a, CP-Q3 (a)) --------------------------------------------
//
// What a statement declares about the relations it reads, and the whole of
// what it declares: `IS` on every relation it **binds**, asked by the
// compiler between the name and the schema, held for **the statement** and
// released when this object dies. Nothing finer: until CP-S2a the outermost
// walk also took `IS` on the slice it had reached, moved at every page
// boundary (AR2-R14, E12) - a position for a mover to wait on, and no mover
// exists (`heap-and-tuple.md` §7.1), so nothing read it; E12's unit was
// un-ratified on that ground, and a mover re-adds a position unit when one
// exists (`txn.md` §5). Nothing about visibility passes through here - the
// snapshot decides what a reader sees, as it always has (AN-S2) - and
// nothing about permission: this is a declaration, published so that an
// operation which changes where a key lives can wait for the reader rather
// than run out from under it. The one such operation is DDL's relation `X`,
// which meets this at the relation entry by the intention rule.
//
// **A read borrow never refuses a read and never makes one wait.** Every
// ask is a non-queueing `TryAcquire`; a refused ask leaves the reader
// holding nothing on that relation and reading on. That is sound because
// the reader needs no borrow to be correct: no page of a dropped relation
// is freed while a statement that could have bound it still runs - the
// statement epoch it published at its head (`drop-table.md` DT1, BF-R9) -
// and the oid is never reissued (DT2); every catalog row is an MVCC version
// the reader's view filters, and no DDL moves data (`alter.md`) - so a plan
// compiled while a DDL's `X` stood reads a snapshot-consistent past, never a
// torn present. It is also what keeps a reader out of the wait-for graph,
// which is why a DDL waiting for one cannot be in a cycle through it
// (AO-S6e-b's section).
//
// **So the defence the bind-time ask gives is one-directional, and it is
// stated as such.** Once the `IS` is *granted*, no DDL can take the
// relation `X` until the statement ends - a reader that has resolved a
// schema is not overtaken, for every relation it binds (AT-S1). When the
// ask is *refused* the DDL was there first, and the three facts above, not
// the lock, are what make the reader's answer right: "a stale parse cannot
// be executed" (`ar0-5-amendment-uniformity.md` §8) is true of the first
// direction only (AT-7 item 10).
//
// The cap is not special-cased for the same reason: `TryAcquire` refuses
// at `max_locks_per_txn` and the reader carries on. The ledger holds one
// entry per bound relation, which no statement approaches.

namespace kds::server {

class ReadBorrow final : public exec::PositionSink {
public:
    // `holder` is 0 where there is no table, and then this is inert: the
    // read path constructs one unconditionally so the call sites carry no
    // branch of their own. `taken` counts granted relation asks, which is
    // what a cell reads to say the borrow was taken at all.
    ReadBorrow(txn::LockTable* locks, std::uint64_t holder, std::uint64_t* taken) noexcept
        : locks_(holder != 0 ? locks : nullptr), holder_(holder), taken_(taken) {}
    // Declaring `rel` at once: what a write does at its resolve. A write
    // resolves a schema exactly as a read does and its own `IX` is taken
    // after that - before its admission for an `INSERT`, at its declared
    // borrow or first qualifying row for an `UPDATE` or `DELETE` (AT-S5e) -
    // so it declares the relation before the schema read, as a
    // `SELECT`'s bind does - an `IS`, compatible with that `IX`, so the
    // writer pays nothing for it. `INSERT`, `UPDATE` and `DELETE` are the
    // three callers; the two with a `WHERE` then hand the borrow to
    // `CompileWhere` for their subquery relations.
    ReadBorrow(txn::LockTable* locks, std::uint64_t holder, std::uint64_t* taken,
               catalog::Oid rel) noexcept
        : ReadBorrow(locks, holder, taken) {
        Position(rel);
    }
    ReadBorrow(const ReadBorrow&) = delete;
    ReadBorrow& operator=(const ReadBorrow&) = delete;
    ~ReadBorrow() override {
        if (locks_ != nullptr) locks_->Release(holder_, holdings_);
    }

    void Position(catalog::Oid rel) override {
        if (locks_ == nullptr) return;
        // The unit a `DROP TABLE` meets, and since CP-S2a the whole of the
        // borrow. **Asked once per relation, whatever the answer**: the
        // record is of the ask and not of the grant, because a reader that
        // was refused - a DDL holds the relation `X` - would otherwise
        // re-ask at every declaration, taking a partition latch each time
        // for a statement that has already decided to run without the
        // borrow. One record per relation rather than one slot, since a
        // statement binds every relation it joins or nests before its walk
        // opens.
        if (std::find(asked_.begin(), asked_.end(), rel) != asked_.end()) return;
        asked_.push_back(rel);
        auto took = locks_->TryAcquire(holder_, txn::LockKey::Relation(rel),
                                       txn::LockMode::kIntentionShared, holdings_);
        if (took.ok() && took.value() && taken_ != nullptr) ++*taken_;
    }

private:
    txn::LockTable* locks_ = nullptr;
    std::uint64_t holder_ = 0;
    std::uint64_t* taken_ = nullptr;
    txn::LockHoldings holdings_;
    // Every relation asked for, whatever the answer. A vector and a linear
    // scan: the count is the statement's relation count, small in practice
    // and never large enough for the scan to show, consulted once per
    // declaration.
    std::vector<catalog::Oid> asked_;
};

// The holder id a read borrow is taken under, and the one spelling of its
// layout: bit 63 is `kReadHolderBit` (`lock_table.hpp`), bits 32-61 the
// core, bits 0-31 the dispatcher's sequence. **Bit 62 is always 0**: one
// minter per core cannot collide with itself. The remote-step minter it
// named retired (AT-S10); its history is at cf889b63. A wait names a holder "a positioned reader" rather than a
// transaction that never existed. `seq` is 32 bits so the field cannot
// alias a core's.
constexpr std::uint64_t ReadHolderId(std::uint32_t core_id, std::uint32_t seq) noexcept {
    return txn::kReadHolderBit | (static_cast<std::uint64_t>(core_id & 0x3FFFFFFF) << 32) | seq;
}

}  // namespace kds::server
