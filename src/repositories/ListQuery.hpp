/**
 * @file ListQuery.hpp
 * @brief Safe, allowlist-based sort/filter helpers for list endpoints — the
 *        next wall every real resource hits after plain (limit, offset)
 *        pagination, and an injection footgun the moment a sort column is
 *        hand-concatenated into SQL.
 *
 *        safe_order_by() validates a client-supplied `?sort=` against a
 *        compile-time column allowlist and returns a sql ORDER BY fragment, or
 *        the fallback when the input isn't allowed — so the column name can
 *        NEVER be attacker-controlled SQL. (Values still bind as parameters.)
 *
 * Usage in a repo on top of CrudBase:
 *     static constexpr std::array kSortable{"created_at", "name"};
 *     auto order = Repositories::safe_order_by(sort_param, kSortable, "created_at DESC");
 *     // SELECT ... ORDER BY <order> LIMIT $1 OFFSET $2
 *
 * Part of the data-layer ergonomics conventions; see CrudBase.hpp.
 */

#pragma once

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>

namespace Repositories {

/**
 * @brief Resolve a client `sort` parameter to a safe "<col> <ASC|DESC>" SQL
 *        fragment. Accepts "col" (asc) or "-col" (desc). Returns @p fallback
 *        verbatim if the column isn't in @p allowed (or the input is empty),
 *        so an unknown/hostile column can never reach the query.
 */
template <typename Allowed>
inline std::string safe_order_by(std::string_view sort, const Allowed& allowed, const std::string& fallback) {
    if (sort.empty())
        return fallback;
    bool desc = false;
    if (sort.front() == '-') {
        desc = true;
        sort.remove_prefix(1);
    }
    const bool ok = std::any_of(std::begin(allowed), std::end(allowed), [&](std::string_view c) { return c == sort; });
    if (!ok)
        return fallback;  // not allow-listed → ignore the client's column entirely
    return std::string(sort) + (desc ? " DESC" : " ASC");
}

/// Overload for a brace list: safe_order_by(sort, {"created_at", "name"}, "...").
inline std::string safe_order_by(std::string_view sort,
                                 std::initializer_list<std::string_view> allowed,
                                 const std::string& fallback) {
    return safe_order_by<std::initializer_list<std::string_view>>(sort, allowed, fallback);
}

}  // namespace Repositories
