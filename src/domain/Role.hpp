/**
 * @file Role.hpp
 * @brief Role + Permission bitmask. flask-base parity: app/models/user.py.
 *
 * The bit layout is intentionally identical to flask-base so a future
 * mixed-language deploy (or a port) can share role rows without remapping.
 * Add new permissions by carving out unused bits (0x02, 0x04, ...). Keep
 * ADMINISTER = 0xff so existing admin checks automatically authorise any
 * permission you add — until you explicitly want a permission that admins
 * shouldn't have, in which case carve it out and document it here.
 */

#pragma once

#include <cstdint>
#include <pqxx/pqxx>
#include <string>

#include <nlohmann/json.hpp>

namespace Domain {

/**
 * @brief Permission bits. Combine with bitwise OR.
 *
 * Style note: lower-case k-prefixed constants follow the rest of the
 * codebase (see kPrincipalAttr in security/Auth.hpp). The flask-base
 * original uses UPPER_CASE; we map them in PATTERNS-FROM-FLASK-BASE.md.
 */
namespace Permission {
inline constexpr std::uint32_t kNone = 0x00;
inline constexpr std::uint32_t kGeneral = 0x01;    // flask-base: GENERAL
inline constexpr std::uint32_t kAuditRead = 0x02;  // read the audit trail (GET /api/admin/audit)
// ADMINISTER is a DEDICATED sentinel bit, not 0xff "all bits": with the old
// 0xff a role that merely accumulated the eight low feature bits would
// ACCIDENTALLY satisfy is_admin (a privilege-escalation footgun). Bit 30
// (0x40000000) is reserved for admin and is never used by a feature permission;
// carve feature bits from the low range (0x04, 0x08, …). Bit 31 is avoided so
// the value fits the signed INTEGER `permissions` column. Existing admin rows
// (seeded as 0xff) are migrated in migrations/004_admin_permission_sentinel.sql.
inline constexpr std::uint32_t kAdminister = 0x40000000;  // flask-base: ADMINISTER (dedicated bit)
}  // namespace Permission

/**
 * @brief Role row. Mirrors the `roles` table.
 */
struct Role {
    int id{0};
    std::string name;
    std::uint32_t permissions{0};
    bool is_default{false};
    std::string created_at;

    template <typename Row>
    static Role from_row(const Row& row) {
        Role r;
        r.id = row["id"].template as<int>();
        r.name = row["name"].template as<std::string>();
        r.permissions = static_cast<std::uint32_t>(row["permissions"].template as<int>());
        r.is_default = row["is_default"].template as<bool>();
        r.created_at = row["created_at"].template as<std::string>();
        return r;
    }

    bool has(std::uint32_t perm) const { return (permissions & perm) == perm; }
    bool is_admin() const { return has(Permission::kAdminister); }
};

inline void to_json(nlohmann::json& j, const Role& r) {
    j = nlohmann::json{
        {"id", r.id},
        {"name", r.name},
        {"permissions", r.permissions},
        {"is_default", r.is_default},
    };
}

}  // namespace Domain
