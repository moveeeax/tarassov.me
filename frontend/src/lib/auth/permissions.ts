/**
 * Permission bit catalogue — THE frontend mirror of Domain::Permission::k*
 * in src/domain/Role.hpp on the backend. Single source: the Permission
 * constants and the admin Roles page checkbox list are both derived from
 * here. Add a row when the C++ side carves out a new bit.
 */

import type { User } from '../api/types';

export interface PermissionBit {
  bit: number;
  label: string;
  hint?: string;
}

export const PERMISSION_BITS: PermissionBit[] = [
  { bit: 0x01, label: 'General', hint: 'Baseline access for any signed-in user' },
  { bit: 0x02, label: 'Audit read', hint: 'Read the audit trail (GET /api/admin/audit)' },
  // Bits 0x04..0x80 are NOT carved out on the backend yet — only kGeneral
  // (0x01), kAuditRead (0x02) and kAdminister (0x40000000, a dedicated sentinel
  // bit) exist in Domain::Permission (src/domain/Role.hpp). Add a row here the moment you
  // define a new kPermission bit there; do not expose checkboxes for bits the
  // backend can't authorise.
];

/** Named bits — mirrors Domain::Permission::k* exactly. */
export const Permission = {
  None: 0x00,
  General: 0x01,
  /** Read the audit trail — Domain::Permission::kAuditRead. */
  AuditRead: 0x02,
  /** Dedicated admin sentinel bit — Domain::Permission::kAdminister. NOT 0xff:
   *  a role that merely accumulates the low feature bits must not become admin. */
  Administer: 0x40000000,
} as const;

/**
 * Minimal user shape userCan() actually inspects: just the role's permission
 * bitmask. Accepting this (rather than the full User) lets callers pass a
 * narrowed session slice without an `as` cast.
 */
export type PermissionUser = Pick<User, 'role'>;

/** True when the user's role carries ALL requested permission bits. */
export function userCan(user: PermissionUser | null | undefined, permission: number): boolean {
  if (!user || !user.role) return false;
  const have = user.role.permissions;
  // The admin sentinel satisfies every permission check — admins can do
  // anything (mirrors C++ Security::Auth::current_user_can).
  if ((have & Permission.Administer) === Permission.Administer) return true;
  return (have & permission) === permission;
}

export function userIsAdmin(user: User | null | undefined): boolean {
  return userCan(user, Permission.Administer);
}
