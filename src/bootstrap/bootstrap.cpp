#include "kds/bootstrap/bootstrap.hpp"

namespace kds::bootstrap {

StatusOr<BootstrapResult> BootstrapDatabase(storage::PageStore& store,
                                             std::uint64_t now_unix_seconds) {
    auto existing = store.Get(server::kSuperBlockPageId);

    if (existing.ok()) {
        auto decoded = server::SuperBlock::Decode(existing.value());
        if (!decoded.ok()) {
            // A page already lives at kSuperBlockPageId but isn't a valid
            // SuperBlock image. Refuse to guess: silently treating this as
            // "fresh" could clobber a real, just-differently-corrupted
            // database; the caller needs to investigate.
            return decoded.status();
        }

        server::SuperBlock sb = decoded.value();
        sb.MarkMounted(now_unix_seconds);
        sb.Encode(existing.value());

        // Existing database: the catalog's fixed pages are assumed to
        // already be there. Catalog::Bootstrap() is deliberately NOT
        // called here - see the file-level comment on why running it
        // again would be destructive.
        return BootstrapResult{sb, catalog::Catalog(store)};
    }

    if (existing.status().code() != StatusCode::kNotFound) {
        return existing.status();
    }

    // Fresh database: create and persist a brand-new SuperBlock, then
    // bootstrap the catalog's fixed pages on top of the same store.
    auto created = store.CreateAt(server::kSuperBlockPageId);
    if (!created.ok()) return created.status();

    server::SuperBlock sb = server::SuperBlock::CreateFresh(now_unix_seconds);
    sb.Encode(created.value());

    catalog::Catalog catalog(store);
    if (Status s = catalog.Bootstrap(); !s.ok()) {
        return s;
    }

    return BootstrapResult{sb, std::move(catalog)};
}

}  // namespace kds::bootstrap
