import { describe, expect, it } from 'vitest';

import type { User } from '../api/types';
import { Permission, PERMISSION_BITS, userCan, userIsAdmin } from './permissions';

function userWith(permissions: number): User {
  return {
    id: 'u1',
    email: 'a@b.c',
    full_name: 'A',
    confirmed: true,
    role_id: 1,
    role: { id: 1, name: 'r', permissions, is_default: false },
  } as User;
}

describe('permission bitmask (mirror of Domain::Permission)', () => {
  it('Administer is a dedicated sentinel bit, disjoint from the feature bits', () => {
    // A single reserved high bit — NOT 0xff. A role that merely accumulates the
    // low feature bits must never become admin (the privilege-escalation footgun
    // the sentinel fixes).
    expect(Permission.Administer).toBe(0x40000000);
    expect(Permission.Administer & (Permission.Administer - 1)).toBe(0); // exactly one bit
    for (const b of PERMISSION_BITS) {
      expect(Permission.Administer & b.bit).toBe(0); // overlaps no feature bit
    }
  });

  it('userCan requires ALL requested bits (non-admin)', () => {
    expect(userCan(userWith(Permission.General), Permission.General)).toBe(true);
    expect(userCan(userWith(Permission.General), Permission.Administer)).toBe(false);
    expect(userCan(userWith(0x03), 0x02)).toBe(true);
    expect(userCan(userWith(0x03), 0x05)).toBe(false); // 0x04 missing
  });

  it('the admin sentinel satisfies every permission check', () => {
    const admin = userWith(Permission.Administer);
    expect(userIsAdmin(admin)).toBe(true);
    expect(userCan(admin, Permission.AuditRead)).toBe(true); // admin bypass
    expect(userCan(admin, 0x04)).toBe(true); // even a bit not in its own mask
  });

  it('userIsAdmin needs the sentinel bit, not accumulated low bits', () => {
    expect(userIsAdmin(userWith(Permission.Administer))).toBe(true);
    expect(userIsAdmin(userWith(0xff))).toBe(false); // all eight low bits ≠ admin
    expect(userIsAdmin(null)).toBe(false);
  });

  it('no role / no user → no permissions', () => {
    expect(userCan(null, Permission.General)).toBe(false);
    expect(userCan({ ...userWith(1), role: undefined } as unknown as User, 1)).toBe(false);
  });
});
