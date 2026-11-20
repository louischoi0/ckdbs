#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "kds/catalog/oid.hpp"

// **What a statement bound** (DI,
// `instructions/v3.2.0/workorder-di-shape-identity.md` DI-Q3): the relations
// its names resolved to, and the namespace its unqualified names were
// resolved in.
//
// A fetch id is a hash of a statement's text and holds neither
// (`parser/fingerprint.hpp`), so one text is two statements wherever a name
// in it resolves to two relations: under two namespaces (`USE`, NS5, NS8),
// or over a relation dropped and created again (a new oid, DT2). A structure
// that names a statement by its fetch id alone sums the two. The binding is
// what tells them apart.
//
// ---- What it holds, and why that is exact -----------------------------------
//
// `oids`: every relation the statement declared at a bind
// (`exec::PositionSink`), **distinct, in first-declared order**.
// `namespace_oid`: the namespace an unqualified name of the statement was
// resolved in, 0 where it wrote none.
//
// Not the relation of every written reference, in written order, as a
// captured order's key holds them (`exec::CapturedKey`): a compile can run
// twice (`CommandDispatcher::ResolveNames`), a query layer statement binds
// one relation once per walk, and a write binds its target and its
// subqueries through separate compiles, so the count of a relation's
// declarations is no function of the text. The distinct list is, and with
// the namespace it loses nothing: two runs of one text under one default
// namespace resolve every name through one map from names to relations at a
// time, two names are one relation only when they are one name, so the
// repeats of the list fall where the text's repeats do, and equal distinct
// lists are equal resolutions of every reference. Without the namespace
// `a.t JOIN b.t JOIN t` under `USE a` and under `USE b` would be one list
// and two statements.
//
// **Two things the dispatcher keeps true for that to hold.** The list is
// one resolution's: a resolve made a second time because a DDL moved under
// it forgets what the first declared (a `SELECT`'s compile starts its
// run's binding afresh; `Rewind`, in `ResolveNames`, for a write) - two
// relations that swapped names between the two would otherwise leave one
// list for two resolutions. And the order of first declaration is a
// function of the text: the compiler declares a block's relations in
// written order before any join search, a write's subqueries compile in a
// fixed order, and a query layer statement's are its compile's describes -
// its walks, which run or not by the data, declare nothing
// (`CommandDispatcher::WalkForLayer`).
//
// **So two statements that resolved any reference differently never compare
// equal**; the other direction is not promised - a statement whose bind was
// refused part way holds the relations declared by then.
//
// ---- The cap -----------------------------------------------------------------
//
// `kMaxRelations` distinct relations, the width of a captured order's key
// (`exec::kMaxPlannedRelations`; the dispatcher asserts they are one
// number). A statement that declares more has a binding the engine does not
// hold: `over` is set, everything else is cleared, and every such statement
// of a shape compares equal - one row that says its binding is not known,
// never a row shown as a binding's (DI-R2).

namespace kds::stats {

struct StatementBinding {
    static constexpr std::size_t kMaxRelations = 16;

    catalog::Oid namespace_oid = 0;
    std::uint8_t n = 0;
    bool over = false;
    std::array<catalog::Oid, kMaxRelations> oids{};

    bool operator==(const StatementBinding&) const = default;

    // A statement that declared nothing: refused before its bind, or naming
    // no relation.
    bool none() const noexcept { return n == 0 && !over; }

    // Whether `rel` is one of the relations held.
    bool Holds(catalog::Oid rel) const noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            if (oids[i] == rel) return true;
        }
        return false;
    }

    // One declaration. `unqualified_in` is the namespace the name was
    // resolved in where it was written unqualified, 0 for a qualified name.
    void Declare(catalog::Oid rel, catalog::Oid unqualified_in) noexcept {
        if (over) return;
        if (unqualified_in != 0) namespace_oid = unqualified_in;
        for (std::size_t i = 0; i < n; ++i) {
            if (oids[i] == rel) return;
        }
        if (n == kMaxRelations) {
            *this = StatementBinding{};
            over = true;
            return;
        }
        oids[n++] = rel;
    }

    // Takes `other`'s declarations after this one's own, in their order:
    // what one compile of a statement bound, added to what the statement
    // had bound before it.
    void Absorb(const StatementBinding& other) noexcept {
        if (other.over) {
            *this = StatementBinding{};
            over = true;
            return;
        }
        for (std::size_t i = 0; i < other.n; ++i) Declare(other.oids[i], 0);
        if (!over && other.namespace_oid != 0) namespace_oid = other.namespace_oid;
    }

    // What `Rewind` returns to: the binding as it stood before a resolve
    // that is about to be made again.
    struct Mark {
        catalog::Oid namespace_oid = 0;
        std::uint8_t n = 0;
        bool over = false;
    };
    Mark mark() const noexcept { return Mark{namespace_oid, n, over}; }
    // Forgets every declaration made since `at`. **Not past an overflow**:
    // a binding that went over since `at` stays over - what it held before
    // is gone, and unknown is the answer that claims nothing.
    void Rewind(const Mark& at) noexcept {
        if (over) return;
        for (std::size_t i = at.n; i < n; ++i) oids[i] = 0;
        n = at.n;
        namespace_oid = at.namespace_oid;
    }
};

// 8 (the namespace) + 1 + 1 + 6 of padding + 16 * 8: what a statistics key
// and a `DispatchOutcome` each carry of it.
static_assert(sizeof(StatementBinding) == 144);

}  // namespace kds::stats
