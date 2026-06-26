# Patterns lifted from flask-base

This template draws its account/admin/email surface from
[hack4impact/flask-base](https://github.com/hack4impact/flask-base) — a
Flask SSR template with a battle-tested user flow. We keep a depth-1 clone
under `_reference/flask-base/` (gitignored) so future contributors can
diff against the original whenever a behaviour question comes up.

This document is the **authoritative list of what we lifted, why, and
what we deliberately changed**. Edit it whenever the divergence grows.

## What flask-base gave us

| Slice | flask-base file(s) | What we use |
|---|---|---|
| Account flow (12 routes) | `app/account/views.py` | Register / login / logout / confirm-email / reset-password / change-email / change-password / manage-profile / unconfirmed / join-from-invite |
| Admin flow (10 routes) | `app/admin/views.py` | Dashboard / new-user / invite-user / list users / user info / change role / change email / delete |
| Domain models | `app/models/user.py` | `User` (id, email, first/last name, password_hash, confirmed, role_id) + `Role` (name, permissions bitmask, default flag) |
| RBAC | `app/models/user.py` `Permission`, `User.can()`, `app/decorators.py` `admin_required` | `Permission::GENERAL = 0x01`, `Permission::ADMINISTER = 0xff`; `User::can(perm) = (role.permissions & perm) == perm` |
| Token-based flows | `User.generate_*_token` + `confirm_account/change_email/reset_password` (itsdangerous) | HMAC-signed timed tokens for confirm / reset / change-email |
| Email pipeline | `app/email.py` + `app/templates/account/email/` | `.html` + `.txt` per template, rendered with Jinja-like engine, sent over SMTP |
| Bootstrap CLI | `manage.py` (`setup_general`, `add_fake_data`, `recreate_db`) | One-shot `--setup-dev` / `--seed-fake N` / `--create-admin EMAIL` flags on the binary |
| Frontend page set | `app/templates/account/`, `app/templates/admin/` | Login, Register, Manage, ResetPassword, Unconfirmed, Admin Dashboard, Users list, User detail, Invite |
| Admin guard pattern | `app/decorators.py` | Server-side role-guard middleware + frontend `<ProtectedRoute requirePermission={Permission.Administer}>` |

## What we deliberately changed

### 1. SSR → SPA + JSON API
flask-base renders Jinja2 server-side. We split:
- **Backend** — JSON-only `/api/*` (already the convention in this template).
- **Frontend** — separate React SPA in `frontend/` consuming the JSON API.

Why: it's the only way to make the frontend "an actually independent
project" you asked for; lets the backend stay a pure API; lets the
frontend deploy independently to a CDN.

### 2. Cookie session → JWT in HttpOnly cookies
flask-base uses Flask-Login (server-side session, signed cookie pointing
at it). We use:
- **Access JWT** — short-lived (≤15 min), `__Host-access` HttpOnly cookie,
  `SameSite=Lax`, `Secure` in prod.
- **Refresh JWT** — longer-lived (7d), `__Host-refresh` HttpOnly cookie,
  rotated on every `/api/auth/refresh`. Refresh-token-id stored in Redis
  for revocation on logout.

Why: stateless API, easier horizontal scaling; HttpOnly + SameSite removes
the standard XSS / CSRF surface that JWT-in-localStorage suffers from.

### 3. WTForms server-rendered → react-hook-form + zod
Validation lives once on the backend (`Api::Validation::*`) and is
duplicated in the frontend through zod schemas generated from the OpenAPI
spec (`openapi-typescript` codegen). Backend remains the source of truth;
the duplicate exists only to give instant client-side feedback.

### 4. Flask-RQ → existing Jobs/Tasks subsystem
flask-base sends emails through a Redis Queue. We already have
`src/jobs/Jobs.hpp` + `src/tasks/Tasks.hpp`. Account email-send is a job
(`type=account_email`, handled by `src/email/AccountEmailWorker.hpp`) so we
get DLQ, retries, and metrics for free. Routing through the queue is the
default (`mail.via_jobs=true`); it falls back to an inline send when Jobs is
off or the enqueue fails.

### 5. SQLite/SQLAlchemy → Postgres/libpqxx
Same model shape, different storage. Migrations live as numbered SQL
files under `migrations/` instead of Alembic.

### 6. Semantic UI → Tailwind + shadcn/ui
flask-base ships Semantic UI + jQuery. We picked Tailwind + shadcn/ui —
modern Radix-based primitives — so the SPA doesn't carry a jQuery
runtime. The page _layout_ (nav, flashes-equivalent, form structure)
mirrors flask-base; the visual primitives are different.

If you'd rather have the original Semantic UI look, `semantic-ui-react`
is a drop-in: keep the same routes/pages, swap the `<Button>` imports.

### 7. `itsdangerous` → libsodium-signed timed tokens
flask-base's confirm/reset/change-email tokens come from
`itsdangerous.TimedJSONWebSignatureSerializer` (HMAC-SHA256 + JSON
payload + expiry). We do the same primitive in C++:
- payload = JSON `{"sub": "<user-id>", "purpose": "confirm|reset|change_email", "exp": <unix>, ...}`
- HMAC-SHA256 over `base64url(payload)` keyed with a secret derived from
  `JWT_SECRET` + a per-purpose salt
- emitted as `base64url(payload).base64url(sig)`, exactly the same shape
  Flask emits

This is _not_ a JWT — JWTs are used for session tokens. These are
short-lived single-purpose link tokens, not session credentials.

### 8. `Permission.ADMINISTER = 0xff` → identical
We keep flask-base's bitmask exactly so a future "moderator" /
"editor" role can be added by carving out bits without breaking the
contract.

## What we did NOT lift

- **`EditableHTML` model + CKeditor admin-WYSIWYG**: out of scope for an
  API template; pulls a 5 MB editor bundle for one feature. Add in a
  separate ADR if you actually need it.
- **Flask-SSLify**: not needed — TLS termination lives at ingress
  (nginx / cert-manager).
- **Flask-Compress**: gzip is ingress-side too.
- **Flask-Assets bundles**: replaced by Vite's bundler.
- **Raygun / Segment / Google Analytics**: those are user choices, not
  template defaults.

## File-level mapping

| flask-base | This repo |
|---|---|
| `app/__init__.py` (factory) | `src/core/Core.hpp` |
| `app/account/views.py` | `src/api/AuthController.hpp` + `src/api/AccountController.hpp` |
| `app/account/forms.py` | `src/api/Validation.hpp` (existing) + zod schemas in `frontend/src/lib/schemas/` |
| `app/admin/views.py` | `src/api/AdminController.hpp` |
| `app/decorators.py` | `Security::Auth::require_role` / Guards macros (`API_REQUIRE_ADMIN`) + `<ProtectedRoute>` in frontend |
| Flask-Login session | `src/security/Jwt.hpp` (access/refresh JWT) + `src/security/SessionCookies.hpp` (HttpOnly cookie sessions) |
| `app/models/user.py` | `src/domain/User.hpp` + `src/domain/Role.hpp` + `src/repositories/{User,Role}Repository.hpp` |
| `app/email.py` | `src/email/Mailer.hpp` + `src/email/Templates.hpp` + `src/email/AccountEmails.hpp` + `src/email/AccountEmailWorker.hpp` (queued send) |
| `app/templates/account/email/*` | `templates/email/*.{html,txt}` |
| `app/templates/account/*.html` | `frontend/src/pages/{Login,Register,Manage,ResetPassword,...}.tsx` |
| `app/templates/admin/*.html` | `frontend/src/pages/admin/*.tsx` |
| `manage.py` `setup_general` | `--setup-dev` flag in `main.cpp` |
| `manage.py` `add_fake_data` | `--seed-fake N` flag in `main.cpp` |
| `manage.py` `run_worker` | `cpp_api_template_worker` binary (already exists) |

## How to keep this honest

When something diverges from flask-base, update the table above. When
flask-base behaviour is the spec, leave a comment in the C++ file:

```cpp
// flask-base parity: app/account/views.py:33 — login() validates email
// then password and surfaces the same generic 'Invalid email or password'
// to defeat user enumeration.
```

That way a future reader can diff the two without trial and error.
