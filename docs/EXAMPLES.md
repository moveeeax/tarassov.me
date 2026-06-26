# End-to-end CRUD example

The template is **not** bare. It already ships a working auth / RBAC / admin
domain you can read as the canonical pattern:

- `migrations/001_users_and_roles.sql` — `users` + `roles` + permission bitmask
- `src/domain/{User,Role}.hpp` — typed DTOs + `nlohmann::to_json`
- `src/repositories/{User,Role}Repository.hpp` — all SQL, typed exceptions
- `src/api/{Auth,Account,Admin}Controller.hpp` — ~20 routes registered in
  `src/api/Endpoints.hpp`

**`src/repositories/UserRepository.hpp` is the canonical, real-world example**
of the repo + domain + cache-aside pattern — read it first. This doc is a
*second*, self-contained walkthrough for adding **your own** new resource on
top of what already ships, without touching the auth tables.

To avoid colliding with the shipped `users` table and the `001` migration, the
walkthrough below introduces a fresh `posts` resource and starts at migration
`002`. Adapt the name to your project.

Target directory layout (adding a new `posts` resource alongside the shipped code):

```
migrations/
  001_users_and_roles.sql      # already shipped — do NOT rewrite
  002_posts.sql                # your new resource
src/
  api/
    PostsController.hpp         # HTTP controller
  domain/
    Post.hpp                   # Typed DTO + nlohmann::to_json
  repositories/
    PostRepository.hpp         # All SQL lives here
tests/
  integration/
    test_posts.cpp             # Exercises the stack end-to-end
```

The `Domain` / `Repositories` namespaces below match the shipped code — the
new types (`Domain::Post`, `Repositories::PostRepository`) just join the same
namespaces. Rename `Post` to whatever your resource is.

---

## 1. Migration

`migrations/002_posts.sql` (run `scripts/new-migration.sh posts` to get the
next-numbered skeleton — `001` is already taken by the shipped auth schema):

```sql
-- pgcrypto (gen_random_uuid) is already enabled by migration 001, but
-- CREATE EXTENSION IF NOT EXISTS is idempotent, so re-declaring is harmless.
CREATE EXTENSION IF NOT EXISTS "pgcrypto";

CREATE TABLE IF NOT EXISTS posts (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    title      VARCHAR(255) NOT NULL,
    body       TEXT NOT NULL DEFAULT '',
    -- Tie a post back to its author. ON DELETE CASCADE so removing a user
    -- removes their posts (adjust to your needs).
    author_id  UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    published  BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_posts_author_id ON posts (author_id);

-- Reuse the same updated_at trigger pattern as 001. The function name is
-- per-table to avoid clobbering users_touch_updated_at().
CREATE OR REPLACE FUNCTION posts_touch_updated_at() RETURNS TRIGGER AS $$
BEGIN NEW.updated_at = now(); RETURN NEW; END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS posts_touch_updated_at ON posts;
CREATE TRIGGER posts_touch_updated_at BEFORE UPDATE ON posts
    FOR EACH ROW EXECUTE FUNCTION posts_touch_updated_at();
```

`MigrationRunner` applies `002` after `001` on boot, in numeric order (or run
`--verify-migrations` to check drift). Do NOT add `BEGIN`/`COMMIT` — the runner
wraps each file in one transaction under an advisory lock (see the note at the
top of `001_users_and_roles.sql`).

---

## 2. Typed DTO

`src/domain/Post.hpp` (the shipped `src/domain/User.hpp` is the real version of
this pattern — open it side by side):

```cpp
#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

namespace Domain {

struct Post {
    std::string id;
    std::string title;
    std::string body;
    std::string author_id;
    bool published{false};
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Post from_row(const Row& row) {
        Post p;
        p.id = row["id"].template as<std::string>();
        p.title = row["title"].template as<std::string>();
        p.body = row["body"].template as<std::string>();
        p.author_id = row["author_id"].template as<std::string>();
        p.published = row["published"].template as<bool>();
        p.created_at = row["created_at"].template as<std::string>();
        p.updated_at = row["updated_at"].template as<std::string>();
        return p;
    }
};

inline void to_json(nlohmann::json& j, const Post& p) {
    j = nlohmann::json{
        {"id", p.id}, {"title", p.title}, {"body", p.body},
        {"author_id", p.author_id}, {"published", p.published},
        {"created_at", p.created_at}, {"updated_at", p.updated_at},
    };
}

}  // namespace Domain
```

Notes:
- Timestamps stay as strings — the DB hands us ISO8601 and the API echoes
  the same shape, so no chrono round-trip is needed.
- Nullable columns become `std::optional<std::string>` and the `to_json`
  branch picks `nullptr` for empty optionals so the wire format stays stable.

---

## 3. Repository

`src/repositories/PostRepository.hpp`. This is exactly the shape
`./scripts/new-resource.sh Post` generates: extend `CrudBase`, declare four
constants, and hand-write only the bespoke writes. `CrudBase`
(`src/repositories/CrudBase.hpp`) supplies `find(id)` / `list(limit, offset)` /
`count()` from those constants, so you don't re-implement the mechanical reads.
The shipped `RoleRepository` is the production version of this exact pattern;
`UserRepository` is a hand-written variant (it joins roles, so it keeps its own
queries).

```cpp
#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include "database/Database.hpp"
#include "domain/Post.hpp"
#include "repositories/CrudBase.hpp"
#include "repositories/RepoErrors.hpp"  // NotFoundError / ConflictError bases
#include "repositories/SqlErrors.hpp"   // detail::translate_sql, shipped helper

namespace Repositories {

// Derive from the generic bases so Api::with_repo_errors maps them to the right
// status (404 / 409) WITHOUT the shared handler knowing this concrete type.
struct PostNotFound : NotFoundError {
    PostNotFound() : NotFoundError("post") {}
};

class PostRepository : public CrudBase<PostRepository, Domain::Post, std::string> {
public:
    // CrudBase supplies find(id) / list(limit, offset) / count() from these four.
    static constexpr const char* kTable    = "posts";
    static constexpr const char* kColumns  = "id, title, body, author_id, published, created_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy  = "created_at DESC";

    // Only the writes are bespoke. Wrap UNIQUE/FK-tripping writes in
    // detail::translate_sql so a SQLSTATE becomes a typed exception, not a 500.
    Domain::Post create(const std::string& title, const std::string& body, const std::string& author_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                std::string("INSERT INTO posts (title, body, author_id) VALUES ($1, $2, $3) RETURNING ") + kColumns,
                title, body, author_id);
            return Domain::Post::from_row(r[0]);
        });
    }

    // Hard delete — the template has NO soft-delete (no deleted_at / is_active).
    // For soft-delete: add a `deleted_at TIMESTAMPTZ` column and filter
    // `WHERE deleted_at IS NULL` in every read.
    void remove(const std::string& id) {
        Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("DELETE FROM posts WHERE id = $1 RETURNING id", id);
            if (r.empty()) throw PostNotFound{};
            return 0;
        });
    }
};

}  // namespace Repositories
```

**Per-user (owner-scoped) resources.** If a `Post` belongs to a user, generate
it with `./scripts/new-resource.sh Post --owned`. The migration gets an
`owner_id` FK, the repo declares `static constexpr const char* kOwnerColumn =
"owner_id";` (which unlocks CrudBase's `find_owned(id, owner)` /
`list_owned(owner, …)` / `count_owned(owner)`), and the controller gates with
`API_REQUIRE_OWNER` and passes the caller as the owner — so one user can **never**
read or delete another's rows. Reaching for the plain `find`/`list` on a
user-owned table is an IDOR; the owner-scoped methods exist so you don't.

Key rules the repository enforces, not the controller:

- Every SQL string for the `posts` table lives here — controllers never touch
  pqxx. The lambda takes `[&](auto& txn)`: `execute_read` / `execute_write` hand
  it a `detail::TracingTxn&`, not a raw `pqxx::work&` / `pqxx::read_transaction&`.
- Constraint violations surface as typed exceptions deriving from
  `NotFoundError` / `ConflictError` (`repositories/RepoErrors.hpp`); translate
  SQLSTATE with `detail::translate_sql` (`repositories/SqlErrors.hpp`), exactly
  like `UserRepository::create` maps `23505` → `DuplicateEmail`. The HTTP layer
  then maps them to 404 / 409 without string-sniffing or knowing the type.
- `find` returns `std::optional` — the controller decides 404 vs cache miss.
- Use `execute_read_primary` (not `execute_read`) right after a write that the
  same request re-reads — replica lag can otherwise return a stale / not-found row.

---

## 4. Thin controller

`src/api/PostsController.hpp` (abbreviated — get + create only). The shipped
`AdminController.hpp` is the real-world version of a guarded CRUD controller:

```cpp
#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

// Controllers do NOT include "api/Api.hpp" — that header includes every
// controller, so pulling it back in is a circular include.
#include "api/Guards.hpp"        // API_REQUIRE_ADMIN / API_REQUIRE_PRINCIPAL / API_REQUIRE_JOBS_READY
#include "api/RequestUtils.hpp"  // is_valid_uuid, parse_int, parse_page_params, ...
#include "api/Validation.hpp"
#include "cache/Cache.hpp"
#include "domain/Post.hpp"
#include "repositories/PostRepository.hpp"
#include "database/Database.hpp"
#include "security/Auth.hpp"        // Security::Auth::principal_of
#include "utils/ErrorResponse.hpp"  // Response::ok / Response::created, ErrorResponse::*

namespace Api {

using namespace drogon;

class PostsController : public HttpController<PostsController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PostsController::createPost, "/api/posts", Post);
    ADD_METHOD_TO(PostsController::getPostById, "/api/posts/{1}", Get);
    METHOD_LIST_END

    void getPostById(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& id) {
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_uuid", "UUID format is invalid"));
            return;
        }
        if (!Database::is_initialized()) {
            callback(ErrorResponse::service_unavailable("database_unavailable", "Database not initialized"));
            return;
        }

        // Cache-aside, fail-open: a Redis hiccup must never block the read
        // (the CONVENTIONS gotchas). CacheManager already swallows redis errors
        // internally; the is_initialized() guard covers the not-booted case.
        const std::string cache_key = "post:" + id;
        if (Cache::is_initialized()) {
            if (auto cached = Cache::get().get(cache_key)) {
                callback(Response::ok({{"data", json::parse(*cached)}, {"source", "cache"}}));
                return;
            }
        }

        auto found = Repositories::PostRepository{}.find(id);
        if (!found) {
            callback(ErrorResponse::not_found("post"));
            return;
        }
        json data(*found);
        if (Cache::is_initialized())
            Cache::get().set(cache_key, data.dump(), /*ttl=*/300);
        callback(Response::ok({{"data", data}, {"source", "database"}}));
    }

    void createPost(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback) {
        // Mutating endpoint: gate it. API_REQUIRE_PRINCIPAL resolves the
        // authenticated principal into `me` (a std::optional<AuthPrincipal>) or
        // rejects with 401 and returns. Drop this only if the route is
        // intentionally public (and say why). Use API_REQUIRE_ADMIN instead
        // for admin-only routes.
        API_REQUIRE_PRINCIPAL(req, callback, me);
        json body;
        if (!Validation::parse_body(req, body, callback)) return;

        Validation::Errors errs;
        Validation::require(errs, body, "title");
        Validation::string_length(errs, body, "title", 1, 255);
        if (errs.any()) { callback(Validation::response_400(errs)); return; }

        // The author is the logged-in principal, not a client-supplied field.
        // AuthPrincipal::subject holds the user id (see src/security/Auth.hpp).
        const std::string author_id = me->subject;

        auto p = Repositories::PostRepository{}.create(
            body["title"].get<std::string>(),
            body.value("body", std::string{}),
            author_id);
        callback(Response::created({{"data", json(p)}, {"message", "Post created"}}));
    }
};

}  // namespace Api
```

Controller responsibilities — and nothing else:

1. Parse + validate the request.
2. Delegate to the repository.
3. Cache-aside where it helps (inline via `Cache::get()`, fail-open).
4. Translate typed exceptions into HTTP status codes.
5. Serialize the DTO back out via `to_json`.

`Response::ok` / `Response::created` live in `utils/ErrorResponse.hpp`
(namespace `Response`); `is_valid_uuid` is an inline helper in
`src/api/RequestUtils.hpp` (alongside `parse_int`, `clamp_int`,
`parse_page_params`, `normalize_path_for_metrics`). The HTTP middleware
wired in `Api::register_controllers()` (bodies in `src/api/Middleware.hpp`)
handles tracing spans — handlers don't open their own.

Two wiring steps the drift checker holds you to:

1. `#include` the controller from `src/api/Api.hpp` so Drogon picks it up.
2. Add a row to `Api::get_endpoints()` in **`src/api/Endpoints.hpp`** (the
   route registry, moved out of `Api.hpp`) and a matching path block in
   `docs/openapi.yaml` — one row per `ADD_METHOD_TO`. Skip either and
   `scripts/check-openapi-drift.sh` turns CI red.

---

## 5. Integration test

`tests/integration/test_posts.cpp` (sketch):

```cpp
#include <gtest/gtest.h>
#include "api/PostsController.hpp"
#include "test_helpers.hpp"

class PostsIntegration : public TestHelpers::CoreBackedTest {
protected:
    bool requires_postgres() const override { return true; }
};

TEST_F(PostsIntegration, CreateAndFetch) {
    // CoreBackedTest boots Core once (override config_overrides() /
    // post_init() as needed) and skips the suite when requires_postgres()
    // can't be satisfied. POST /api/posts via TestHelpers::make_request,
    // GET it back, assert equality.
}
```

The template ships `TestHelpers::CoreBackedTest` (with `config_overrides`,
`requires_postgres`, `post_init` hooks), `TestHelpers::make_request(method[, body])`,
`truncate_users()`, and `drain_jobs({types})` — lean on those instead of
hand-rolling a fixture.

Note: `truncate_users()` wipes the **shipped auth `users` table** — it is not a
generic helper. For your `posts` resource you need a matching truncate of your
own table, e.g.:

```cpp
inline void truncate_posts() {
    Database::get().execute_write([](auto& txn) {
        txn.exec("TRUNCATE TABLE posts");
        return 0;
    });
}
```

(`posts.author_id` references `users(id)` with `ON DELETE CASCADE`, so wiping
`users` also clears dependent `posts` — but a dedicated `truncate_posts()`
keeps the intent explicit and works even if you drop the FK.)

---

## Why keep this as a doc instead of shipping a second demo controller?

- The shipped surface stays focused: auth / RBAC / admin is the one real
  domain in `src/`, so grep surveys and static analysis don't return throwaway
  demo noise on top of it.
- `docs/openapi.yaml` tracks only endpoints that actually exist, so spec-drift
  checks don't fight sample data that was never wired up.
- The `Post` walkthrough lives here, fully copy-pasteable, so you add it (at
  migration `002`) when you need it instead of deleting it when you don't.

What **is** shipped and ready to read as the canonical pattern:
`migrations/001_users_and_roles.sql`, `src/domain/{User,Role}.hpp`,
`src/repositories/{User,Role}Repository.hpp`, and
`src/api/{Auth,Account,Admin}Controller.hpp` (routes in
`src/api/Endpoints.hpp`).
