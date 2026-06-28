/**
 * @file test_auth_permissions.cpp
 * @brief Guards the admin permission sentinel — no infra.
 *
 * The Auth layer duplicates the admin bitmask (it sits below Domain and can't
 * include it), and the bit must be a single dedicated high bit, not 0xff. These
 * unit tests make both invariants un-driftable.
 */

#include <cstdint>

#include <gtest/gtest.h>

#include "domain/Role.hpp"
#include "security/Auth.hpp"

// The two duplicated copies of the sentinel must stay equal.
TEST(AuthPermissions, AdminSentinelMatchesDomain) {
    EXPECT_EQ(Security::Auth::kAdminPermissionBits, Domain::Permission::kAdminister);
}

// A SINGLE dedicated bit, disjoint from the low feature range, fitting INTEGER —
// so a role that ORs together feature bits can never accidentally be admin.
TEST(AuthPermissions, AdminSentinelIsASingleReservedBit) {
    const std::uint32_t bits = Domain::Permission::kAdminister;
    EXPECT_NE(bits, 0u);
    EXPECT_EQ(bits & (bits - 1u), 0u) << "kAdminister must be exactly one bit";
    EXPECT_EQ(bits & 0xffu, 0u) << "must not overlap the low feature range (0x01..0x80)";
    EXPECT_LE(bits, 0x7fffffffu) << "must fit the signed INTEGER permissions column";
}

// is_admin must require the sentinel bit, not be satisfied by accumulating the
// low feature bits — the exact escalation the old 0xff sentinel allowed.
TEST(AuthPermissions, AccumulatedLowBitsAreNotAdmin) {
    Domain::Role r;
    r.permissions = 0xffu;  // all eight low bits set
    EXPECT_FALSE(r.is_admin());

    r.permissions = Domain::Permission::kAdminister;
    EXPECT_TRUE(r.is_admin());

    r.permissions = Domain::Permission::kGeneral | Domain::Permission::kAdminister;
    EXPECT_TRUE(r.is_admin());
}
