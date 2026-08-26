#include "kds/bootstrap/bootstrap.hpp"

#include "kds/storage/tagged_cell.hpp"

namespace kds::bootstrap {

StatusOr<BootstrapResult> BootstrapDatabase(storage::PageStore& store,
                                             std::uint64_t now_unix_seconds,
                                             std::uint32_t inline_cell_width,
                                             std::uint32_t cores, Logger* log) {
    // Checked before anything is read or created: an illegal width must not
    // be the reason a fresh database gets pinned to a number no build can
    // use, and it must not be reported as a *mismatch* below when it is
    // really a bad setting.
    if (Status s = storage::CheckInlineCellWidth(inline_cell_width); !s.ok()) {
        return s;
    }
    // Same rule, same reason (workplan-crosscore.md M6).
    if (Status s = server::CheckCoreCount(cores); !s.ok()) {
        return s;
    }

    auto existing = store.Get(server::kSuperBlockPageId);

    if (existing.ok()) {
        auto decoded = server::SuperBlock::Decode(existing.value().bytes());
        if (!decoded.ok()) {
            if (log != nullptr && log->enabled(LogLevel::kError)) {
                // The refusal below is the right behavior and a confusing
                // one to meet without context: the database is not missing,
                // it is unreadable, and nothing was touched.
                log->Error("bootstrap", "a page exists at the superblock id but does not "
                                        "decode as a superblock; refusing to treat this as a "
                                        "fresh database: " + decoded.status().message());
            }
            // A page already lives at kSuperBlockPageId but isn't a valid
            // SuperBlock image. Refuse to guess: silently treating this as
            // "fresh" could clobber a real, just-differently-corrupted
            // database; the caller needs to investigate.
            return decoded.status();
        }

        server::SuperBlock sb = decoded.value();

        // The pinned-width check (docs/rules/rule-fixed-length-tuple.md section
        // 4). On-disk tuple layout depends on the width, so running with a
        // different one would not fail - it would decode every row at the
        // wrong offsets. Both numbers are named because the operator's next
        // question is always "which one is the database's?".
        if (sb.inline_cell_width() != inline_cell_width) {
            Status mismatch = Status::InvalidArgument(
                "inline_cell_width " + std::to_string(inline_cell_width) +
                " does not match the " + std::to_string(sb.inline_cell_width()) +
                " this database was created with; the width is pinned at bootstrap because "
                "tuple layout depends on it, and rewriting existing data for a new width is "
                "unsupported");
            if (log != nullptr && log->enabled(LogLevel::kError)) {
                log->Error("bootstrap", mismatch.message());
            }
            return mismatch;
        }

        // The pinned-core-count check (docs/inflight/in-progress/workplan-crosscore.md M6). Both
        // numbers are named for the reason the width's message gives, and
        // the message says what the operator's actual options are - because
        // unlike the width, this one has a legitimate fix that is not a
        // rebuild: run with the count the database was created for.
        if (sb.core_count() != cores) {
            Status mismatch = Status::InvalidArgument(
                "cores " + std::to_string(cores) + " does not match the " +
                std::to_string(sb.core_count()) +
                " this database was created with; WAL streams are per core, so mounting under a "
                "different count would leave streams with nothing to replay them - restart with "
                "cores = " + std::to_string(sb.core_count()) + ", or create a new database");
            if (log != nullptr && log->enabled(LogLevel::kError)) {
                log->Error("bootstrap", mismatch.message());
            }
            return mismatch;
        }

        sb.MarkMounted(now_unix_seconds);
        sb.Encode(existing.value().bytes());
        if (log != nullptr && log->enabled(LogLevel::kInfo)) {
            log->Info("bootstrap", "mounted existing database, superblock version " +
                                       std::to_string(sb.version()) + ", inline_cell_width " +
                                       std::to_string(sb.inline_cell_width()) + ", cores " +
                                       std::to_string(sb.core_count()) +
                                       "; catalog bootstrap skipped");
        }

        // Existing database: the catalog's fixed pages are assumed to
        // already be there. Catalog::Bootstrap() is deliberately NOT
        // called here - see the file-level comment on why running it
        // again would be destructive.
        catalog::Catalog catalog(store, sb.inline_cell_width(), sb.core_count());
        catalog.SetLogger(log);
        return BootstrapResult{sb, std::move(catalog)};
    }

    if (existing.status().code() != StatusCode::kNotFound) {
        return existing.status();
    }

    // Fresh database: create and persist a brand-new SuperBlock, then
    // bootstrap the catalog's fixed pages on top of the same store. This is
    // the one moment the cell width is chosen; every mount after this one
    // validates against it.
    auto created = store.CreateAt(server::kSuperBlockPageId);
    if (!created.ok()) return created.status();

    server::SuperBlock sb =
        server::SuperBlock::CreateFresh(now_unix_seconds, inline_cell_width, cores);
    sb.Encode(created.value().bytes());
    if (log != nullptr && log->enabled(LogLevel::kInfo)) {
        log->Info("bootstrap", "no superblock found; creating a fresh database (version " +
                                   std::to_string(sb.version()) + ", inline_cell_width " +
                                   std::to_string(inline_cell_width) + ", cores " +
                                   std::to_string(cores) + ")");
    }

    catalog::Catalog catalog(store, inline_cell_width, cores);
    catalog.SetLogger(log);
    if (Status s = catalog.Bootstrap(); !s.ok()) {
        return s;
    }

    return BootstrapResult{sb, std::move(catalog)};
}

}  // namespace kds::bootstrap
