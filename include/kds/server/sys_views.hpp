#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// **The `sys` views' registry** (`docs/spec/sys-views.md`;
// `instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` SV1-SV5).
//
// Every `sys.<name>` a `SELECT` can read is one entry here, and every
// `SHOW` target is one `SELECT` over one of them (SV1): the entry is the
// only place a view's identity lives, and its builder - a
// `CommandDispatcher` member in `src/server/sys_views.cpp` - is the only
// place its data logic lives (SV4).
//
// ---- The key a view may declare (BO-Q12) ----------------------------------
//
// A view cannot always be materialized whole: `sys.page_slots` would read
// every page of the volume. And `DESCRIBE` refuses a name that does not
// exist, which a filter over a materialized view would answer with zero
// rows - a refusal turned into an empty answer. So a view may declare one
// key column:
//
//   kRequired  a `SELECT` without the conjunct `key = <literal>` is
//              refused, and the builder reads only the row(s) it names;
//   kChecked   the conjunct is optional; when written, the builder is told
//              and refuses a key that names nothing.
//
// The builder takes the first such conjunct as a bound on what it reads and
// returns only the rows it names, so that conjunct is not compared against
// them again: `rel_name = 'ns.t'` names the rows whose `rel_name` is `t`.
// Every other conjunct, a second one on the key included, filters as usual.

namespace kds::server {

enum class ViewKeyMode : std::uint8_t { kNone, kChecked, kRequired };

struct SysViewSpec {
    std::string_view name;
    ViewKeyMode key_mode = ViewKeyMode::kNone;
    std::string_view key_column;
    // The key's declared type, which its literal is coerced to before the
    // builder sees it (`docs/spec/types.md` §3.1a).
    std::uint32_t key_type = 0;
};

// The entry for `name`, case-insensitive, or null.
const SysViewSpec* FindSysView(std::string_view name) noexcept;

// Every view's name, in registry order, for a refusal that lists them.
std::vector<std::string> SysViewNames();

// `t` or `ns.t` in a raw argument, split at its one dot (AF-T3): the
// qualifier (empty when none) and the name. Shared by the debug surface's
// bare-token commands and the views that take a relation name as their key.
std::pair<std::string_view, std::string_view> SplitQualifiedArg(std::string_view arg);

}  // namespace kds::server
