# Removing the reference / demo material

This template ships some **pedagogical** content that exists to explain *why* the
code is shaped the way it is. A real fork doesn't need it. This file says exactly
what is reference-only (safe to delete) and what is the actual application (keep).

## TL;DR

```bash
# Strips the reference material as part of initialising your fork:
./scripts/init-project.sh --no-demo my-service docker.io/myorg example.org

# …or remove it by hand at any time:
rm -rf _reference docs/PATTERNS-FROM-FLASK-BASE.md
```

Both do the same thing; `--no-demo` also scrubs the now-dangling doc links.

## What is reference-only (safe to delete)

| Path | What it is | Why it's removable |
| --- | --- | --- |
| `_reference/flask-base/` | ~21 MB of the upstream Python **flask-base** source. | The C++ app mirrors its patterns (auth flows, permission bitmask, email tokens); the source is here only so you can diff behaviour. Nothing builds or imports it. |
| `docs/PATTERNS-FROM-FLASK-BASE.md` | The mapping doc: "flask-base did X → here it's Y". | Pure narrative. No code references it at runtime. |

Deleting these does **not** touch any C++ target, migration, test, Helm chart, or
CI job — `init-project.sh` already excludes `_reference/` from its renaming pass,
and `--no-demo` only removes the two paths above.

## What is NOT a demo — keep it

The application itself is production scaffolding, not a showcase. Keep all of it:

- **Auth**: register / login / logout / refresh, JWT + session cookies, password
  hashing, email-confirmation & reset tokens.
- **Domain**: `User` / `Role` / `AuditEntry` and their repositories — this is your
  real user system, not sample data. The admin API and audit trail are real.
- **Infra seams**: jobs/worker, cache, rate-limiting, idempotency, the resource
  scaffolder (`scripts/new-resource.sh`), Helm charts, and the CI pipeline.

If you want a *truly* minimal start, delete your own unused **feature** code
(controllers/repositories you scaffold and abandon) — but the auth/User/Role/Audit
core is the point of the template.

> Note: the "demo" Docker images and the `env-stage` deployment referenced in the
> CHANGELOG are *deployment* showcases (published images, a staging namespace),
> independent of the source tree. Removing the reference material above has no
> effect on them.
