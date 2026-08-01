#include "kds/exec/budget.hpp"

namespace kds::exec {

Status Budget::Exhausted() const {
    // The message names the limit and the key that changes it, because the
    // operator reading it is deciding one of two things: whether the
    // statement is wrong, or whether the ceiling is. Neither is answerable
    // from "budget exceeded".
    return Status::ResourceExhausted(
        "statement exceeded its row-touch budget of " + std::to_string(limit_) +
        " tuples; it was still reading. A correlated subquery or a join on a non-pk "
        "column reads the inner relation once per outer row, which is quadratic - "
        "rewrite it to bind a primary key, or raise `max_rows_touched` in the config "
        "if this statement is meant to cost that much");
}

}  // namespace kds::exec
