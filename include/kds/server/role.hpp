#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "kds/base/status.hpp"

// The authorization model (docs/spec/protocol.md §14, decided 2026-08-13):
// three roles ordered by inclusion, held per authenticated user, checked
// once per statement against the class the dispatcher routes on.
//
//   readonly   SELECT/SHOW/DESCRIBE/ANALYZE, PING, transaction control,
//              and the session's own SET ISOLATION
//   readwrite  everything readonly may, plus INSERT/UPDATE/DELETE
//   admin      everything, including DDL, STOP, SYNC and server-wide SET
//
// Ranks, not grant sets, on purpose: v1's question is "how much may this
// connection do", and a total order answers it in one comparison with no
// storage beyond a word in the users file. Per-relation grants remain
// the future refinement (same §14) and slot in *under* this check - a
// rank decides whether the statement class is admissible at all, a grant
// would then decide it for the named relation - so nothing here is
// foreclosed.
//
// With `auth = off` every session holds kAdmin (session.hpp): an
// unauthenticated instance is the operator's own process, and refusing
// it anything would be authorization theater.

namespace kds::server {

enum class Role : std::uint8_t {
    kReadOnly = 0,
    kReadWrite = 1,
    kAdmin = 2,
};

// The one comparison the model reduces to. Written as a named function
// rather than a bare `>=` at call sites, because the *ordering* is the
// design decision and this is its single home.
constexpr bool RoleCovers(Role held, Role required) noexcept {
    return static_cast<std::uint8_t>(held) >= static_cast<std::uint8_t>(required);
}

constexpr std::string_view RoleName(Role role) noexcept {
    switch (role) {
        case Role::kReadOnly: return "readonly";
        case Role::kReadWrite: return "readwrite";
        case Role::kAdmin: return "admin";
    }
    return "?";
}

// Exact, lowercase spellings only - a users file is security state, and
// "ReadOnly" silently meaning readonly is a lenience nobody audits.
inline StatusOr<Role> ParseRole(std::string_view text) {
    if (text == "readonly") return Role::kReadOnly;
    if (text == "readwrite") return Role::kReadWrite;
    if (text == "admin") return Role::kAdmin;
    return Status::InvalidArgument("role must be readonly, readwrite or admin, got '" +
                                   std::string(text) + "'");
}

}  // namespace kds::server
