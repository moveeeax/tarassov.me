# Migrations

Drop numbered `.sql` files here — the template's `MigrationRunner` picks them
up on boot, applies any that aren't already in `schema_migrations`, and
records what it did. Naming: `NNN_description.sql` (e.g. `001_users.sql`).

A starter schema lives in [`docs/EXAMPLES.md`](../docs/EXAMPLES.md) if you
want a worked example.

## Generating a new migration

```bash
./scripts/new-migration.sh add_users_table
# ==> Created migrations/001_add_users_table.sql
```

The script picks the next free `NNN`, slugifies the description, and writes a
`BEGIN; ... COMMIT;` skeleton.

## Ops

- `./tarassov_me --verify-migrations` — list pending files without
  applying (useful as a CI gate; exits 1 if any are pending).
- `./tarassov_me --run-migrations` — apply pending migrations and exit
  (CLI-flag form of `RUN_MIGRATIONS_ONLY=1`; equivalent to `make migrate-local`
  for the native binary).
- `DB_MIGRATIONS_ENABLED=false` — skip running migrations on app boot
  (set this when an init container is responsible instead).
- `RUN_MIGRATIONS_ONLY=1 ./tarassov_me` — env-var equivalent of
  `--run-migrations`, convenient for Helm init-containers.
- `make migrate` (Docker) / `make migrate-local` (native) — wrappers around
  the above.

## Seed data

Optional fixtures live in `migrations/seeds/*.sql`. They are **never** run
automatically — apply them manually with `make seed` after the schema is up.
Use them for dev-only test users, sample rows, etc. Do not encode anything
the schema relies on; that belongs in a numbered migration.

## Ignored

Anything outside the top level of this directory is skipped by the runner,
so the `seeds/` subfolder and any `archive/` directory won't be auto-applied
on boot.
