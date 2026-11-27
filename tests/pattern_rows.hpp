#pragma once

// **A test's view of `sys.patterns` by fetch id alone** (DI-S3).
//
// A row is a shape under what its `WAYSTONE ON` bound (`rows.hpp`), so the
// catalog's lookups take the binding beside the fetch id. A cell that opts
// one statement in and then reads "its" row knows the text and not the
// oids, and for such a cell a fetch id still names one row: these helpers
// find it by scanning, as the catalog's own lookup did before the binding
// was part of the key. **A scan each call**: a cell that counts the opted
// map's fills or misses asks the catalog directly, with a binding.

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/stats/statement_binding.hpp"
#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::testing_patterns {

// A binding for a cell that writes rows by hand and runs no statement. A
// relation every catalog holds, `sys.tables`: a mount ends the opt-ins
// naming a relation that is gone, and these cells are not about that.
inline stats::StatementBinding AnyBinding() {
    stats::StatementBinding binding;
    binding.Declare(catalog::kSysTablesTable, 0);
    return binding;
}

// The first current-version row of `fetch_id`, whatever it bound.
inline StatusOr<catalog::SysPatternRow> PatternRowOf(catalog::Catalog& catalog,
                                                     std::uint64_t fetch_id) {
    auto rows = catalog.ListPatterns();
    if (!rows.ok()) return rows.status();
    for (const catalog::SysPatternRow& row : rows.value()) {
        if (row.fetch_id != fetch_id) continue;
        if (!parser::IsCurrentFingerprintVersion(row.fingerprint_version)) continue;
        return row;
    }
    return Status::NotFound("no current sys.patterns row for this fetch_id");
}

// The opted entry of that row, as `Catalog::FindOptedPattern` answers it.
inline StatusOr<const catalog::PatternAccess*> OptedPatternOf(catalog::Catalog& catalog,
                                                              std::uint64_t fetch_id) {
    auto row = PatternRowOf(catalog, fetch_id);
    if (!row.ok()) return row.status();
    return catalog.FindOptedPattern(fetch_id, catalog::BindingOf(row.value()));
}

// That row's oid, 0 where there is none: what `TrailRecorder::OnPatternResult`
// is handed.
inline catalog::Oid OptedRowOf(catalog::Catalog& catalog, std::uint64_t fetch_id) {
    auto row = PatternRowOf(catalog, fetch_id);
    return row.ok() ? row.value().oid : 0;
}

// Children an interior directory page names; -1 where it does not read. A
// mount's renewed root names none, whether it is a new page or a stale
// directory's page reused (DV-S2) - so "renewed" is this, not a new id.
inline int DirChildren(storage::PageStore& store, PageId page) {
    auto bytes = store.GetForRead(page);
    if (!bytes.ok()) return -1;
    int n = 0;
    for (std::size_t i = 0; i < stats::kDirFanout; ++i) {
        if (stats::LoadDirChild(bytes.value().bytes(), i) != stats::kEmptyDirSlot) ++n;
    }
    return n;
}

}  // namespace kds::testing_patterns
