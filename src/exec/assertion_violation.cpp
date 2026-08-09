#include "kds/exec/assertion_violation.hpp"

#include "kds/exec/row_codec.hpp"

namespace kds::exec {

std::string AssertionViolationMessage(std::string_view assertion_name,
                                      std::span<const GroupKeyPart> group,
                                      BoundAggregate aggregate, std::string_view sum_column,
                                      std::int64_t enforced_max) {
    std::string out = "assertion \"";
    out += assertion_name;
    out += "\" group (";
    for (std::size_t i = 0; i < group.size(); ++i) {
        if (i != 0) out += ", ";
        out += group[i].column;
        out += '=';
        out += FormatValue(group[i].type_val, group[i].value);
    }
    out += "): ";
    if (aggregate == BoundAggregate::kCount) {
        out += "COUNT(*)";
    } else {
        out += "SUM(";
        out += sum_column;
        out += ')';
    }
    out += " would exceed bound ";
    out += std::to_string(enforced_max);
    return out;
}

}  // namespace kds::exec
