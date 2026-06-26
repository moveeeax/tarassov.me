import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { Permission } from './permissions';

/**
 * Cross-language drift guard: the TS Permission bits in permissions.ts MUST
 * equal Domain::Permission::k* in src/domain/Role.hpp. The whole point of
 * keeping the bit layout identical (so a mixed-language deploy can share role
 * rows) only holds if the two definitions can't silently diverge.
 *
 * Rather than hard-code the C++ numbers a second time, we PARSE Role.hpp and
 * assert the TS side matches. Change a bit on either side and this test goes
 * red — exactly the drift the comment in permissions.ts promises to catch.
 */

const ROLE_HPP = fileURLToPath(new URL('../../../../src/domain/Role.hpp', import.meta.url));

/** Pull `inline constexpr std::uint32_t kName = 0x..;` values out of Role.hpp. */
function parsePermissionBits(src: string): Record<string, number> {
  const out: Record<string, number> = {};
  const re = /inline\s+constexpr\s+std::uint32_t\s+k(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)/g;
  for (let m = re.exec(src); m !== null; m = re.exec(src)) {
    out[m[1]] = Number(m[2]);
  }
  return out;
}

describe('Permission bits mirror Domain::Permission (src/domain/Role.hpp)', () => {
  const cpp = parsePermissionBits(readFileSync(ROLE_HPP, 'utf8'));

  it('parsed the C++ constants (sanity)', () => {
    // If Role.hpp moves or the constant style changes, fail loudly here
    // instead of silently passing an empty comparison.
    expect(Object.keys(cpp)).toEqual(expect.arrayContaining(['None', 'General', 'Administer']));
  });

  it('None == kNone', () => {
    expect(Permission.None).toBe(cpp.None);
    expect(Permission.None).toBe(0x00);
  });

  it('General == kGeneral', () => {
    expect(Permission.General).toBe(cpp.General);
    expect(Permission.General).toBe(0x01);
  });

  it('Administer == kAdminister', () => {
    expect(Permission.Administer).toBe(cpp.Administer);
    expect(Permission.Administer).toBe(0x40000000);
  });

  it('every C++ bit is mirrored on the TS side', () => {
    // Catches the "added a new kPermission in Role.hpp, forgot permissions.ts"
    // direction — the one a value-only assertion above would miss.
    const ts = Permission as Record<string, number>;
    for (const [name, value] of Object.entries(cpp)) {
      expect(ts[name], `Permission.${name} missing or wrong in permissions.ts`).toBe(value);
    }
  });
});
