-- 004: admin permission is now a DEDICATED sentinel bit, not 0xff "all bits".
--
-- With ADMINISTER = 0xff, is_admin() is `(permissions & 0xff) == 0xff`, so a
-- role that merely accumulated the eight low feature bits (0x01|0x02|…|0x80)
-- would ACCIDENTALLY become admin — a privilege-escalation footgun the moment a
-- fork defines its eighth permission. Move admin to a reserved high bit
-- (0x40000000 = bit 30; bit 31 is avoided so it fits the signed INTEGER column),
-- which no feature permission ever uses. See src/domain/Role.hpp.
--
-- Convert the seeded admin role (migration 001 inserted it as 0xff = 255). Match
-- by both name and old value so a fork's renamed/repurposed role isn't touched.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction — no BEGIN/COMMIT.

UPDATE roles
SET permissions = 1073741824        -- 0x40000000, Permission::kAdminister
WHERE name = 'Administrator'
  AND permissions = 255;            -- 0xff, the old all-bits admin marker
