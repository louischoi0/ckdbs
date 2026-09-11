#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "kds/catalog/range_directory.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/txn/lock_table.hpp"

// ---- The read borrow (AO-S6e-b; AO-R12, AR2-R14, AR2 §3's `SELECT` row;
// moved to the bind at AT-S1, `workorder-at-m3-uniformity.md` AT-R1) ------
//
// What a statement declares about the relations it reads, and the whole of
// what it declares: `IS` on every relation it **binds**, asked by the
// compiler between the name and the schema, and `IS` on the slice its
// outermost walk has reached, moved at every page boundary - all of it held
// for **the statement** and released when this object dies. Nothing about
// visibility passes through here - the snapshot decides what a reader sees,
// as it always has (AN-S2) - and nothing about permission: this is a
// position, published so that an operation which changes where a key lives
// can wait for the reader rather than run out from under it. In M2 the one
// such operation is DDL's relation `X` (AO-R12: no mover exists), which
// meets this at the relation entry by the intention rule.
//
// **A read borrow never refuses a read and never makes one wait.** Every
// ask is a non-queueing `TryAcquire`; a refused ask leaves the reader
// holding nothing on that relation and reading on. That is sound because
// the reader needs no borrow to be correct: `drop-table.md` DT1 leaves a
// dropped relation's pages allocated and its oid never reissued, every
// catalog row is an MVCC version the reader's view filters, and no DDL
// moves data (`alter.md`) - so a plan compiled while a DDL's `X` stood
// reads a snapshot-consistent past, never a torn present. It is also what
// keeps a reader out of the wait-for graph, which is why a DDL waiting for
// one cannot be in a cycle through it (AO-S6e-b's section).
//
// **So the defence the bind-time ask gives is one-directional, and it is
// stated as such.** Once the `IS` is *granted*, no DDL can take the
// relation `X` until the statement ends - a reader that has resolved a
// schema is not overtaken, which since AT-S1 covers a join's inner
// relation, every subquery relation, a write's own relation and the stage a
// remote step executes, not only the outermost walk. When the ask is
// *refused* the DDL was there first,
// and the three facts above are what make the reader's answer right; the
// lock does not, and `ar0-5-amendment-uniformity.md` §8's "a stale parse
// cannot be executed" is true of the first direction only (AT-7 item 10).
//
// The cap is not special-cased for the same reason: `TryAcquire` refuses
// at `max_locks_per_txn` and the reader carries on. The ledger holds one
// entry per bound relation plus one slice, which no statement approaches.

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
        Position(rel, 0, catalog::kIdSpaceEnd);
    }
    ReadBorrow(const ReadBorrow&) = delete;
    ReadBorrow& operator=(const ReadBorrow&) = delete;
    ~ReadBorrow() override {
        if (locks_ != nullptr) locks_->Release(holder_, holdings_);
    }

    void Position(catalog::Oid rel, std::uint64_t lo, std::uint64_t hi) override {
        if (locks_ == nullptr) return;
        // The intention above the slice, and the unit a `DROP TABLE`
        // meets. **Asked once per relation, whatever the answer**: the
        // record is of the ask and not of the grant, because a reader that
        // was refused - a DDL holds the relation `X` - would otherwise
        // re-ask at every page boundary, taking a partition latch per page
        // for the length of a walk that has already decided to run without
        // a position. One record per relation rather than one slot, since a
        // statement binds every relation it joins or nests before its walk
        // reports anything.
        auto ask = std::find_if(asks_.begin(), asks_.end(),
                                [rel](const Ask& a) { return a.rel == rel; });
        if (ask == asks_.end()) {
            const bool took = Take(txn::LockKey::Relation(rel), txn::LockMode::kIntentionShared);
            if (took && taken_ != nullptr) ++*taken_;
            ask = asks_.insert(asks_.end(), Ask{rel, took});
        }
        if (rel != rel_) {
            rel_ = rel;
            // The slice belonged to the relation being left: a slice kept
            // across the change would be a position declared in a relation
            // this reader is no longer in, and on the refused arm it would
            // be held for the rest of the statement.
            if (slice_.has_value()) {
                locks_->ReleaseOne(holder_, *slice_, txn::LockMode::kIntentionShared, holdings_);
                slice_.reset();
            }
        }
        // No unit without the intention above it (`lock_table.hpp`): a
        // slice borrowed under a relation entry this holder has no `IS` on
        // is invisible to a relation-level ask.
        if (!ask->granted) return;
        // The whole id space is the relation, which is already held - what
        // every bind declares, and what a walk's first report carries.
        if (lo == 0 && hi == catalog::kIdSpaceEnd) return;

        const txn::LockKey slice = txn::LockKey::Slice(rel, lo, hi);
        if (slice_.has_value() && *slice_ == slice) return;
        // **Taken before the previous one is let go**, so the position is
        // never unheld between two pages - which is the one instant a mover
        // could pass through if the order were reversed.
        if (!Take(slice, txn::LockMode::kIntentionShared)) return;
        if (slice_.has_value()) {
            locks_->ReleaseOne(holder_, *slice_, txn::LockMode::kIntentionShared, holdings_);
        }
        slice_ = slice;
    }

private:
    struct Ask {
        catalog::Oid rel;
        bool granted;
    };

    bool Take(const txn::LockKey& key, txn::LockMode mode) {
        auto took = locks_->TryAcquire(holder_, key, mode, holdings_);
        return took.ok() && took.value();
    }

    txn::LockTable* locks_ = nullptr;
    std::uint64_t holder_ = 0;
    std::uint64_t* taken_ = nullptr;
    txn::LockHoldings holdings_;
    // Every relation asked for, with its answer. A vector and a linear
    // scan: the count is the statement's relation count, small in practice
    // and never large enough for the scan to show, and a walk consults it
    // once per page boundary.
    std::vector<Ask> asks_;
    // The relation the *position* is in - the walk's, not the bind's -
    // which is the one the slice below belongs to.
    catalog::Oid rel_ = 0;
    std::optional<txn::LockKey> slice_;
};

// The holder id a read borrow is taken under, and the one spelling of its
// layout: bit 63 is `kReadHolderBit` (`lock_table.hpp`), **bit 62 says
// which minter**, bits 32-61 the core, bits 0-31 that minter's own
// sequence. Two minters live on one core - the dispatcher for statements,
// the remote-step server for the stages it executes - each with a counter
// of its own, and without the minter bit two holders on one core could
// share an id: the table's covering test would then grant the second an
// `IS` it does not own and record nothing for it, so the first holder's
// release would free a position the second was still walking under. A
// wait names any of them "a positioned reader" rather than a transaction
// that never existed. `seq` is 32 bits so the field cannot alias a core's.
constexpr std::uint64_t ReadHolderId(std::uint32_t core_id, std::uint32_t seq,
                                     bool remote) noexcept {
    return txn::kReadHolderBit | (remote ? (std::uint64_t{1} << 62) : 0) |
           (static_cast<std::uint64_t>(core_id & 0x3FFFFFFF) << 32) | seq;
}

}  // namespace kds::server
