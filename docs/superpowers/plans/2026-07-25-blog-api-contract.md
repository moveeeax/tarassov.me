# Blog API Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the public + admin blog API per spec `docs/superpowers/specs/2026-07-25-blog-api-contract-design.md`: server-side filters/facets/adjacent, preview tokens, media library, clean SEO surface from `site.base_url`, delete `/blog-single.html`, migrate the frontend in the same branch.

**Architecture:** Hybrid resource contract (`/public/posts` + `?include=`), breaking v1 in place. Backend C++20/Drogon: repository gains filtered SQL, controller parses `page/topic/tag/q/include`; SSR pages move to file templates (`templates/pages/`, mirroring `templates/email` + `src/email/Templates.hpp`); dates become ISO 8601 at the `from_row` boundary. Frontend (`blog.js`, React admin) consumes the new envelope.

**Tech Stack:** C++20, Drogon, libpqxx, GTest (fixtures in `tests/test_helpers.hpp`), Postgres, vanilla JS public site, React/TS admin (TanStack Query, openapi-typescript).

## Global Constraints

- Branch: implement on `spec/blog-api-contract` (spec already committed there); one PR to `main`.
- Every commit that adds/changes/removes a route updates **both** `src/api/Endpoints.hpp` and `docs/openapi.yaml` in the same commit (CI: `openapi-drift`, `routes-registered`).
- All API dates: ISO 8601 UTC `YYYY-MM-DDTHH:MM:SSZ`. Public list envelope: `{items,page,limit,total[,facets]}` (breaking: was `{data,total,limit,offset}`).
- Public list: `limit` default 10, **max 50**; `page` 1-based. Admin list keeps `{data,total,limit,offset}` envelope.
- No new C++ dependencies. clang-format-17 (`make lint-format`). Tests: `make test` first run, `make test-quick` iterations, `make test-unit` for pure-unit.
- Frontend checks run in docker (host has no node): `docker run --rm -v "$PWD":/w -w /w/frontend node:20 bash -c 'npm ci --no-audit --no-fund && npm run gen:api && npm run typecheck && npm test && npm run lint && npm run build'`.
- Node/npm-generated `frontend/src/lib/api/schema.gen.ts` is regenerated once, in Task 12.
- `/blog-single.html` is deleted everywhere (spec §5.5). Preview never leaks drafts into lists/facets/sitemap.
- PR #5 (`fix/public-pages-authpaths-and-proto`) is superseded by Tasks 6–7 (allowlist + base_url); close it when this PR merges.

---

### Task 1: ISO 8601 dates at the serialization boundary

**Files:**
- Modify: `src/utils/Time.hpp` (add `pg_to_iso8601`, `epoch_to_iso8601`)
- Modify: `src/domain/Post.hpp` (`Post::from_row`, `PostCard::from_row` convert timestamps)
- Test: `tests/unit/test_time_iso.cpp` (create)

**Interfaces:**
- Produces: `Utils::Time::pg_to_iso8601(const std::string&) -> std::string`; `Utils::Time::epoch_to_iso8601(std::int64_t) -> std::string`. Every later task relies on `Post`/`PostCard` timestamps already being ISO.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_time_iso.cpp
#include <gtest/gtest.h>
#include "utils/Time.hpp"

TEST(TimeIso, PgTimestampToIso) {
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25 05:34:32.87643+00"), "2026-07-25T05:34:32Z");
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25 05:34:32+00"), "2026-07-25T05:34:32Z");
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25 05:34:32+00:00"), "2026-07-25T05:34:32Z");
    // Non-UTC offset: keep the offset (still valid ISO 8601), just normalize the shape.
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25 05:34:32+03"), "2026-07-25T05:34:32+03");
    // Already ISO or unexpected shape: returned unchanged.
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25T05:34:32Z"), "2026-07-25T05:34:32Z");
    EXPECT_EQ(Utils::Time::pg_to_iso8601(""), "");
}

TEST(TimeIso, EpochToIso) {
    EXPECT_EQ(Utils::Time::epoch_to_iso8601(0), "1970-01-01T00:00:00Z");
    EXPECT_EQ(Utils::Time::epoch_to_iso8601(1774417272), "2026-03-25T08:21:12Z");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test-unit` — expected: compile error `pg_to_iso8601 is not a member`.

- [ ] **Step 3: Implement in `src/utils/Time.hpp`** (append inside `namespace Utils::Time`)

```cpp
/// "2026-07-25 05:34:32.87643+00[:00]" (libpqxx text form) → ISO 8601.
/// Space→'T', fractional seconds dropped, "+00"/"+00:00"→"Z"; other offsets kept.
/// Inputs that don't look like "YYYY-MM-DD HH:MM:SS..." are returned unchanged.
inline std::string pg_to_iso8601(const std::string& ts) {
    if (ts.size() < 19 || ts[10] != ' ')
        return ts;
    std::string out = ts.substr(0, 10) + "T" + ts.substr(11, 8);
    std::size_t i = 19;
    if (i < ts.size() && ts[i] == '.') {          // fractional seconds
        ++i;
        while (i < ts.size() && ts[i] >= '0' && ts[i] <= '9')
            ++i;
    }
    std::string offset = ts.substr(i);
    if (offset == "+00" || offset == "+00:00" || offset == "Z" || offset.empty())
        out += "Z";
    else
        out += offset;
    return out;
}

/// Unix seconds → "YYYY-MM-DDTHH:MM:SSZ" (UTC).
inline std::string epoch_to_iso8601(std::int64_t secs) {
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
```

Add missing includes to Time.hpp if absent: `<cstdint> <ctime> <string>`.

In `src/domain/Post.hpp` wrap every timestamp read (both `from_row`s) — `#include "utils/Time.hpp"` at top:

```cpp
// Post::from_row:
if (!row["published_at"].is_null())
    e.published_at = Utils::Time::pg_to_iso8601(row["published_at"].template as<std::string>());
e.created_at = Utils::Time::pg_to_iso8601(row["created_at"].template as<std::string>());
e.updated_at = Utils::Time::pg_to_iso8601(row["updated_at"].template as<std::string>());
// PostCard::from_row:
if (!row["published_at"].is_null())
    c.published_at = Utils::Time::pg_to_iso8601(row["published_at"].template as<std::string>());
```

- [ ] **Step 4: Run** `make test-unit` → PASS; then `make test-quick` (no integration regressions — frontend `fmtDate` slices `[0,10)`, unaffected).
- [ ] **Step 5: Commit** `git add -A && git commit -m "feat(api): ISO 8601 timestamps at the Post serialization boundary"`

---

### Task 2: Migration 008 — LeetCode topic backfill

**Files:**
- Create: `migrations/008_backfill_leetcode_topic.sql`
- Test: `tests/integration/test_migrations.cpp` (only if it asserts a migration count/list — check `grep -n "007\|count" tests/integration/test_migrations.cpp` and extend the same way for 008)

- [ ] **Step 1: Write the migration**

```sql
-- Migration 008: backfill_leetcode_topic
-- The public site used to derive a display topic in JS: numeric-slug posts
-- (^\d+-) are LeetCode problem notes. Persist that once so the server owns
-- topic and the JS derivation can be deleted. Idempotent.
UPDATE posts SET topic = 'LeetCode' WHERE slug ~ '^\d+-' AND btrim(topic) = '';
```

- [ ] **Step 2: If `test_migrations.cpp` asserts the applied set, add `008_backfill_leetcode_topic` mirroring how 007 is listed. Run `make test-quick` → PASS (migrations apply on fixture boot).**
- [ ] **Step 3: Add a behavior test in `tests/integration/test_post.cpp`** — create a post with slug `123-two-sum`, topic `""`, then re-run the backfill statement via `Database::get().execute_write` and assert `find` returns topic `LeetCode`:

```cpp
TEST_F(PostsFlowTest, LeetCodeTopicBackfill) {
    json body = {{"slug", "123-two-sum"}, {"title", "Two Sum"}, {"status", "published"}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_EQ(resp->statusCode(), k201Created);
    Database::get().execute_write([](auto& txn) {
        txn.exec("UPDATE posts SET topic = 'LeetCode' WHERE slug ~ '^\\d+-' AND btrim(topic) = ''");
        return 0;
    });
    Repositories::PostRepository repo;
    auto found = repo.find_published_by_slug("123-two-sum");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->topic, "LeetCode");
}
```

- [ ] **Step 4: Run** `make test-quick` → PASS.
- [ ] **Step 5: Commit** `git commit -am "feat(db): migration 008 — persist LeetCode topic for numeric-slug posts"`

---

### Task 3: Public list — server filters, paging, facets

**Files:**
- Modify: `src/repositories/PostRepository.hpp`
- Modify: `src/api/PostsController.hpp` (`publicListPosts`)
- Modify: `src/api/Endpoints.hpp` (description of `GET /api/v1/public/posts` → "List published posts (filterable, paged, optional facets)")
- Modify: `docs/openapi.yaml` (`/api/v1/public/posts` params + new envelope)
- Test: `tests/integration/test_post.cpp`

**Interfaces:**
- Produces: `Repositories::PublicListFilter { std::string topic, tag, q; }`;
  `list_published_cards(const PublicListFilter&, int limit, int offset)`;
  `count_published(const PublicListFilter&) -> long`;
  `struct FacetRow { std::string name; long count; }`;
  `facets(const PublicListFilter&) -> std::pair<std::vector<FacetRow>, std::vector<FacetRow>>` (topics, tags).
- Response envelope consumed by Task 11 (blog.js): `{items,page,limit,total[,facets:{topics[],tags[]}]}`.

- [ ] **Step 1: Failing tests** (seed helper + cases). Add to `test_post.cpp`:

```cpp
// Seed helper for the suite: create a published post via the admin handler.
void seed(const char* slug, const char* title, const char* topic, json tags) {
    json body = {{"slug", slug}, {"title", title}, {"summary", std::string("about ") + title},
                 {"status", "published"}, {"topic", topic}, {"tags", tags}};
    auto r = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_EQ(r->statusCode(), k201Created);
}

TEST_F(PostsFlowTest, PublicListFiltersAndFacets) {
    seed("a-k8s", "Kube A", "Kubernetes", {"kubernetes", "talos"});
    seed("b-k8s", "Kube B", "Kubernetes", {"kubernetes"});
    seed("c-sre", "SLO Burn", "SRE", {"prometheus"});
    seed("d-other", "Notes", "", json::array());

    auto req = TestHelpers::make_request(Get);
    req->setParameter("topic", "Kubernetes");
    req->setParameter("include", "facets");
    auto resp = call([&](auto cb) { controller.publicListPosts(req, std::move(cb)); });
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"], 2);
    EXPECT_EQ(body["items"].size(), 2);
    EXPECT_EQ(body["page"], 1);
    for (auto& it : body["items"]) EXPECT_FALSE(it.contains("body"));
    // Facets are computed over the CURRENT filter.
    EXPECT_EQ(body["facets"]["topics"][0]["name"], "Kubernetes");
    EXPECT_EQ(body["facets"]["topics"][0]["count"], 2);
    EXPECT_EQ(body["facets"]["tags"][0]["name"], "kubernetes");

    // tag filter
    auto req2 = TestHelpers::make_request(Get);
    req2->setParameter("tag", "prometheus");
    auto r2 = call([&](auto cb) { controller.publicListPosts(req2, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(r2->body()))["total"], 1);

    // q searches title+summary
    auto req3 = TestHelpers::make_request(Get);
    req3->setParameter("q", "burn");
    auto r3 = call([&](auto cb) { controller.publicListPosts(req3, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(r3->body()))["total"], 1);

    // Empty topic groups as "Other" and is filterable as such.
    auto req4 = TestHelpers::make_request(Get);
    req4->setParameter("topic", "Other");
    auto r4 = call([&](auto cb) { controller.publicListPosts(req4, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(r4->body()))["total"], 1);
}

TEST_F(PostsFlowTest, PublicListClampsLimitTo50) {
    auto req = TestHelpers::make_request(Get);
    req->setParameter("limit", "1000");
    auto resp = call([&](auto cb) { controller.publicListPosts(req, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(resp->body()))["limit"], 50);
}
```

- [ ] **Step 2: Run** `make test-quick` → FAIL (`items` missing — old envelope).
- [ ] **Step 3: Implement repository.** In `PostRepository.hpp` add above the class:

```cpp
struct PublicListFilter {
    std::string topic;  // exact; "Other" matches empty/blank topic too
    std::string tag;    // exact tag membership
    std::string q;      // ILIKE over title+summary
};
```

Inside the class, a shared WHERE builder + the three queries. pqxx has no named
params, so build the clause and bind positionally via `exec_params` with a
`pqxx::params`-style approach — use string stream + `txn.esc()` for values
(consistent, safe):

```cpp
private:
    // WHERE clause for public reads. Values are escaped via txn.esc; LIKE
    // wildcards in q are escaped so a literal '%' can't scan everything.
    template <typename Txn>
    static std::string public_where(Txn& txn, const PublicListFilter& f) {
        std::string w = "status = 'published'";
        if (!f.topic.empty()) {
            const std::string t = txn.esc(f.topic);
            if (f.topic == "Other")
                w += " AND (btrim(topic) = '' OR topic = 'Other')";
            else
                w += " AND topic = '" + t + "'";
        }
        if (!f.tag.empty())
            w += " AND (',' || tags || ',') LIKE ('%,' || '" + txn.esc(f.tag) + "' || ',%')";
        if (!f.q.empty()) {
            std::string esc_q = f.q;
            // escape LIKE metacharacters, then esc() for SQL
            std::string tmp;
            for (char c : esc_q) { if (c=='%'||c=='_'||c=='\\') tmp += '\\'; tmp += c; }
            const std::string qq = txn.esc(tmp);
            w += " AND (title ILIKE '%" + qq + "%' OR summary ILIKE '%" + qq + "%')";
        }
        return w;
    }
public:
```

New/changed public methods (the old zero-arg overloads are **replaced**; the
sitemap query stays as-is):

```cpp
    std::vector<Domain::PostCard> list_published_cards(const PublicListFilter& f, int limit, int offset) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec(
                "SELECT slug, title, summary, topic, tags, published_at, "
                "GREATEST(1, CEIL(array_length(regexp_split_to_array(trim(body), '\\s+'), 1)::numeric / 200))::int "
                "AS read_mins FROM posts WHERE " + public_where(txn, f) +
                " ORDER BY published_at DESC, id DESC LIMIT " + std::to_string(limit) +
                " OFFSET " + std::to_string(offset));
            std::vector<Domain::PostCard> out;
            out.reserve(r.size());
            for (const auto& row : r) out.push_back(Domain::PostCard::from_row(row));
            return out;
        });
    }

    long count_published(const PublicListFilter& f = {}) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec("SELECT count(*) FROM posts WHERE " + public_where(txn, f));
            return r[0][0].template as<long>();
        });
    }

    struct FacetRow { std::string name; long count; };
    std::pair<std::vector<FacetRow>, std::vector<FacetRow>> facets(const PublicListFilter& f) {
        return Database::get().execute_read([&](auto& txn) {
            std::pair<std::vector<FacetRow>, std::vector<FacetRow>> out;
            auto topics = txn.exec(
                "SELECT COALESCE(NULLIF(btrim(topic), ''), 'Other') AS name, count(*)::bigint AS c "
                "FROM posts WHERE " + public_where(txn, f) +
                " GROUP BY 1 ORDER BY c DESC, name ASC");
            for (const auto& row : topics)
                out.first.push_back({row["name"].template as<std::string>(), row["c"].template as<long>()});
            auto tags = txn.exec(
                "SELECT btrim(t) AS name, count(*)::bigint AS c FROM posts, "
                "LATERAL unnest(string_to_array(tags, ',')) AS t "
                "WHERE " + public_where(txn, f) + " AND btrim(t) <> '' "
                "GROUP BY 1 ORDER BY c DESC, name ASC LIMIT 30");
            for (const auto& row : tags)
                out.second.push_back({row["name"].template as<std::string>(), row["c"].template as<long>()});
            return out;
        });
    }
```

- [ ] **Step 4: Implement controller.** Replace `publicListPosts` body:

```cpp
    void publicListPosts(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        // Hybrid contract (spec 2026-07-25): server-side filters + paging;
        // facets embedded on demand so the index needs exactly one request.
        const int limit = clamp_int(req->getParameter("limit"), 10, 1, 50);
        const int page = clamp_int(req->getParameter("page"), 1, 1, 1000000);
        Repositories::PublicListFilter f;
        f.topic = req->getParameter("topic");
        f.tag = req->getParameter("tag");
        f.q = req->getParameter("q");

        Repositories::PostRepository repo;
        auto items = repo.list_published_cards(f, limit, (page - 1) * limit);
        long total = repo.count_published(f);
        json out = {{"items", json::array()}, {"page", page}, {"limit", limit}, {"total", total}};
        for (const auto& e : items) out["items"].push_back(e);

        const std::string include = req->getParameter("include");
        if (include.find("facets") != std::string::npos) {
            auto [topics, tags] = repo.facets(f);
            json jt = json::array(), jg = json::array();
            for (const auto& t : topics) jt.push_back({{"name", t.name}, {"count", t.count}});
            for (const auto& t : tags) jg.push_back({{"name", t.name}, {"count", t.count}});
            out["facets"] = {{"topics", jt}, {"tags", jg}};
        }
        callback(Response::ok(out));
    }
```

Fix the two existing tests that consumed the old envelope (`CreatePublishAndPublicRead`, `DraftHiddenFromPublic` — change `body["data"]` → `body["items"]`, `total` semantics unchanged).

- [ ] **Step 5: Update `docs/openapi.yaml`** — `/api/v1/public/posts` GET: add query params `page,limit,topic,tag,q,include`; replace response schema with the `items/page/limit/total/facets` envelope (mirror JSON above; facets optional). Update `Endpoints.hpp` description string.
- [ ] **Step 6: Run** `make test-quick` → PASS; `./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh` → OK.
- [ ] **Step 7: Commit** `git commit -am "feat(api)!: public posts list — server filters, 1-based paging (max 50), include=facets"`

---

### Task 4: Adjacent posts in the by-slug read

**Files:**
- Modify: `src/repositories/PostRepository.hpp`, `src/api/PostsController.hpp` (`publicGetPost`), `docs/openapi.yaml`
- Test: `tests/integration/test_post.cpp`

**Interfaces:**
- Produces: `struct AdjacentRef { std::string slug, title; }`; `find_adjacent(const Domain::Post&) -> std::pair<std::optional<AdjacentRef>, std::optional<AdjacentRef>>` (prev=older, next=newer).

- [ ] **Step 1: Failing test**

```cpp
TEST_F(PostsFlowTest, PublicGetIncludesAdjacent) {
    seed("p1", "First", "T", json::array());
    seed("p2", "Middle", "T", json::array());
    seed("p3", "Last", "T", json::array());
    auto req = TestHelpers::make_request(Get);
    req->setParameter("include", "adjacent");
    auto resp = call([&](auto cb) { controller.publicGetPost(req, std::move(cb), "p2"); });
    auto body = json::parse(std::string(resp->body()));
    // Newest-first feed: p3 is newer (next), p1 is older (prev).
    EXPECT_EQ(body["data"]["adjacent"]["next"]["slug"], "p3");
    EXPECT_EQ(body["data"]["adjacent"]["prev"]["slug"], "p1");

    auto reqEdge = TestHelpers::make_request(Get);
    reqEdge->setParameter("include", "adjacent");
    auto rEdge = call([&](auto cb) { controller.publicGetPost(reqEdge, std::move(cb), "p3"); });
    auto bEdge = json::parse(std::string(rEdge->body()));
    EXPECT_TRUE(bEdge["data"]["adjacent"]["next"].is_null());
    EXPECT_EQ(bEdge["data"]["adjacent"]["prev"]["slug"], "p2");
}
```

- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** Repo (`(published_at,id)` row comparison breaks timestamp ties deterministically):

```cpp
    struct AdjacentRef { std::string slug; std::string title; };
    std::pair<std::optional<AdjacentRef>, std::optional<AdjacentRef>> find_adjacent(const std::string& id) {
        return Database::get().execute_read([&](auto& txn) {
            std::pair<std::optional<AdjacentRef>, std::optional<AdjacentRef>> out;
            auto prev = txn.exec_params(  // older
                "SELECT slug, title FROM posts WHERE status='published' AND "
                "(published_at, id) < (SELECT published_at, id FROM posts WHERE id=$1) "
                "ORDER BY published_at DESC, id DESC LIMIT 1", id);
            if (!prev.empty())
                out.first = AdjacentRef{prev[0]["slug"].template as<std::string>(),
                                        prev[0]["title"].template as<std::string>()};
            auto next = txn.exec_params(  // newer
                "SELECT slug, title FROM posts WHERE status='published' AND "
                "(published_at, id) > (SELECT published_at, id FROM posts WHERE id=$1) "
                "ORDER BY published_at ASC, id ASC LIMIT 1", id);
            if (!next.empty())
                out.second = AdjacentRef{next[0]["slug"].template as<std::string>(),
                                         next[0]["title"].template as<std::string>()};
            return out;
        });
    }
```

Controller `publicGetPost` — signature gains the request use:

```cpp
    void publicGetPost(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& slug) {
        Repositories::PostRepository repo;
        auto found = repo.find_published_by_slug(slug);
        if (!found) { callback(ErrorResponse::not_found("post")); return; }
        json data = json(*found);
        if (req->getParameter("include").find("adjacent") != std::string::npos) {
            auto [prev, next] = repo.find_adjacent(found->id);
            data["adjacent"] = {
                {"prev", prev ? json{{"slug", prev->slug}, {"title", prev->title}} : json(nullptr)},
                {"next", next ? json{{"slug", next->slug}, {"title", next->title}} : json(nullptr)}};
        }
        callback(Response::ok({{"data", data}}));
    }
```

- [ ] **Step 4:** openapi.yaml: add `include` query param + `adjacent` in the response schema. Run `make test-quick` + drift scripts → PASS.
- [ ] **Step 5: Commit** `git commit -am "feat(api): public post by-slug — include=adjacent (prev/next), kills the feed refetch"`

---

### Task 5: Draft preview tokens

**Files:**
- Modify: `src/security/Tokens.hpp` (`Purpose::Preview`), `src/repositories/PostRepository.hpp` (`find_by_slug_any`), `src/api/PostsController.hpp` (issue endpoint + preview in `publicGetPost`), `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/integration/test_post.cpp`

**Interfaces:**
- Produces: `POST /api/v1/posts/{id}/preview-token` (admin) → `{"data":{"url":"/blog/<slug>?preview=<tok>","expires_at":"<ISO>"}}`; `?preview=` honored by `publicGetPost` (this task) and `GET /blog/{slug}` (Task 7). Token: purpose `preview`, `sub` = post UUID, TTL 3600 s, secret `Security::Auth::get().config().jwt_secret` (pattern: `src/api/AccountController.hpp:377`).

- [ ] **Step 1: Failing tests**

```cpp
TEST_F(PostsFlowTest, DraftPreviewToken) {
    json body = {{"slug", "wip"}, {"title", "WIP"}, {"status", "draft"}};
    auto created = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    const std::string id = json::parse(std::string(created->body()))["data"]["id"];

    // Draft is invisible without a token.
    auto r404 = call([&](auto cb) { controller.publicGetPost(TestHelpers::make_request(Get), std::move(cb), "wip"); });
    EXPECT_EQ(r404->statusCode(), k404NotFound);

    // Issue a preview token.
    auto issued = call([&](auto cb) {
        controller.previewToken(TestHelpers::make_request(Post), std::move(cb), id);
    });
    ASSERT_EQ(issued->statusCode(), k200OK);
    auto ib = json::parse(std::string(issued->body()));
    const std::string url = ib["data"]["url"];
    ASSERT_NE(url.find("/blog/wip?preview="), std::string::npos);
    const std::string tok = url.substr(url.find("preview=") + 8);

    // Valid token → draft served.
    auto reqTok = TestHelpers::make_request(Get);
    reqTok->setParameter("preview", tok);
    auto rOk = call([&](auto cb) { controller.publicGetPost(reqTok, std::move(cb), "wip"); });
    EXPECT_EQ(rOk->statusCode(), k200OK);

    // Token bound to a DIFFERENT post id → 404.
    const std::string other = Security::Tokens::issue(
        Security::Auth::get().config().jwt_secret, "00000000-0000-0000-0000-000000000000",
        Security::Tokens::Purpose::Preview, std::chrono::seconds(3600));
    auto reqBad = TestHelpers::make_request(Get);
    reqBad->setParameter("preview", other);
    auto rBad = call([&](auto cb) { controller.publicGetPost(reqBad, std::move(cb), "wip"); });
    EXPECT_EQ(rBad->statusCode(), k404NotFound);

    // Expired token → 404.
    const std::string expired = Security::Tokens::issue(
        Security::Auth::get().config().jwt_secret, id,
        Security::Tokens::Purpose::Preview, std::chrono::seconds(-1));
    auto reqExp = TestHelpers::make_request(Get);
    reqExp->setParameter("preview", expired);
    auto rExp = call([&](auto cb) { controller.publicGetPost(reqExp, std::move(cb), "wip"); });
    EXPECT_EQ(rExp->statusCode(), k404NotFound);

    // Drafts never appear in the list even while a preview token exists.
    auto rList = call([&](auto cb) { controller.publicListPosts(TestHelpers::make_request(Get), std::move(cb)); });
    for (auto& it : json::parse(std::string(rList->body()))["items"]) EXPECT_NE(it["slug"], "wip");
}
```

Add includes to the test file: `#include "security/Auth.hpp"` `#include "security/Tokens.hpp"`.

- [ ] **Step 2: Run** → FAIL (`previewToken` missing; `Purpose::Preview` missing).
- [ ] **Step 3: Implement.** Tokens.hpp: add `Preview` to the enum and `case Purpose::Preview: return "preview";` to `purpose_string`. Repo:

```cpp
    std::optional<Domain::Post> find_by_slug_any(const std::string& slug) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Domain::Post> {
            auto r = txn.exec_params(std::string("SELECT ") + kColumns + " FROM posts WHERE slug = $1", slug);
            if (r.empty()) return std::nullopt;
            return Domain::Post::from_row(r[0]);
        });
    }
```

Controller — register + handlers (`#include "security/Auth.hpp"`, `"security/Tokens.hpp"`, `"utils/Time.hpp"`):

```cpp
    // in METHOD_LIST (admin group):
    ADD_METHOD_TO(PostsController::previewToken, "/api/v1/posts/{1}/preview-token", Post);

    static constexpr std::chrono::seconds kPreviewTtl{3600};

    void previewToken(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id) {
        API_REQUIRE_ADMIN(req, callback);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_uuid", "UUID format is invalid"));
            return;
        }
        Repositories::PostRepository repo;
        auto found = repo.find(id);
        if (!found) { callback(ErrorResponse::not_found("post")); return; }
        const auto token = Security::Tokens::issue(Security::Auth::get().config().jwt_secret, id,
                                                   Security::Tokens::Purpose::Preview, kPreviewTtl);
        const auto exp = Utils::Time::epoch_to_iso8601(Utils::Time::now_epoch_seconds() + kPreviewTtl.count());
        callback(Response::ok({{"data", {{"url", "/blog/" + found->slug + "?preview=" + token},
                                         {"expires_at", exp}}}}));
    }
```

`publicGetPost`: before the published lookup, honor a preview token — a shared
helper so Task 7 reuses it verbatim:

```cpp
    // Returns the post for slug honoring an optional ?preview= token: published
    // posts always; a draft only when the token verifies AND is bound to it.
    static std::optional<Domain::Post> resolve_post(const std::string& slug, const std::string& preview) {
        Repositories::PostRepository repo;
        if (preview.empty())
            return repo.find_published_by_slug(slug);
        auto any = repo.find_by_slug_any(slug);
        if (!any) return std::nullopt;
        if (any->status == "published") return any;
        auto vr = Security::Tokens::verify(Security::Auth::get().config().jwt_secret, preview,
                                           Security::Tokens::Purpose::Preview);
        if (vr.ok && vr.sub == any->id) return any;
        return std::nullopt;  // invalid/expired/foreign token behaves like 404
    }
```

and in `publicGetPost` replace the lookup line with
`auto found = resolve_post(slug, req->getParameter("preview"));`.

- [ ] **Step 4:** Endpoints.hpp: add `{"POST", "/api/v1/posts/{id}/preview-token", "Admin: issue a draft preview link"}`. openapi.yaml: path + `preview` query param on the public by-slug. Run `make test-quick` + both drift scripts → PASS.
- [ ] **Step 5: Commit** `git commit -am "feat(api): stateless draft preview tokens (1h TTL) for admin"`

---

### Task 6: `site.base_url` + sitemap cleanup

**Files:**
- Modify: `src/core/Core.hpp` (`validate_config_`), `src/api/PublicPagesController.hpp` (`origin` → config-first; sitemap Cache-Control), `config/config.json`, `config/config.production.json`, `docs/CONFIG.md`
- Modify: `helm/tarassov-me/values.yaml` (+`site.baseUrl`), `helm/tarassov-me/templates/configmap.yaml` (render `"site": {"base_url": ...}` beside the existing `"api"` block, `helm/tarassov-me/templates/configmap.yaml:68`)
- Test: `tests/integration/test_post.cpp`

**Interfaces:**
- Produces: `Api::site_base_url(const HttpRequestPtr&) -> std::string` (free function in `PublicPagesController.hpp`): config `site.base_url`/`SITE_BASE_URL` if non-empty, else header-derived origin (dev fallback). All later URL generation uses it.

- [ ] **Step 1: Failing test**

```cpp
class PublicPagesTest : public TestHelpers::CoreBackedTest {
protected:
    Api::PublicPagesController pages;
    Api::PostsController controller;
    std::string config_file_name() const override { return "public_pages_test_config.json"; }
    void config_overrides(json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["site"]["base_url"] = "https://example.test";
    }
    template <typename Fn> HttpResponsePtr call(Fn&& fn) {
        HttpResponsePtr resp;
        fn([&](const HttpResponsePtr& r) { resp = r; });
        return resp;
    }
};

TEST_F(PublicPagesTest, SitemapUsesConfiguredBaseAndCaches) {
    auto resp = call([&](auto cb) { pages.sitemap(TestHelpers::make_request(Get), std::move(cb)); });
    const std::string body(resp->body());
    EXPECT_NE(body.find("<loc>https://example.test/</loc>"), std::string::npos);
    EXPECT_EQ(body.find("http://"), body.find("http://www.sitemaps.org"));  // no http:// locs
    EXPECT_EQ(resp->getHeader("cache-control"), "public, max-age=3600");
}
```

(Move the existing `SsrSitemapAndRedirect` assertions that still apply into this fixture as needed.)

- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.**

`PublicPagesController.hpp` — replace `origin()` with:

```cpp
    // Canonical origin: site.base_url when configured (prod — validated at
    // boot); header-derived only as a dev fallback with no configured base.
    static std::string site_base_url(const HttpRequestPtr& req) {
        const std::string cfg_base =
            Config::get().get<std::string>("site.base_url", "SITE_BASE_URL", "");
        if (!cfg_base.empty()) {
            // normalize: no trailing slash
            return cfg_base.back() == '/' ? cfg_base.substr(0, cfg_base.size() - 1) : cfg_base;
        }
        const std::string host = req->getHeader("host");
        if (host.empty()) return "https://tarassov.me";
        std::string proto = req->getHeader("x-forwarded-proto");
        if (proto.empty()) proto = "https";
        return proto + "://" + host;
    }
```

(`#include "utils/Config.hpp"`.) Point both handlers at it and add to `sitemap`:
`resp->addHeader("Cache-Control", "public, max-age=3600");`

`Core.hpp` `validate_config_` (inside `if (is_prod)`), spec §3.2 — hard fail:

```cpp
            const std::string base_url = cfg.get<std::string>("site.base_url", "SITE_BASE_URL", "");
            if (base_url.empty() || base_url.rfind("https://", 0) != 0)
                throw std::runtime_error(
                    "Config validation: site.base_url must be a non-empty https:// URL in production — "
                    "sitemap/canonical/OG URLs are generated from it. Set SITE_BASE_URL=https://<domain>.");
```

Config files: add `"site": {"base_url": ""}` to `config/config.json` and
`"site": {"base_url": "https://tarassov.me"}` to `config/config.production.json`.
Helm `values.yaml`: `site:\n  baseUrl: "https://tarassov.me"`; configmap template adds
`"site": {"base_url": {{ .Values.site.baseUrl | quote }} }`. `docs/CONFIG.md`: row
`site.base_url | SITE_BASE_URL | "" (required https URL in prod)`.

- [ ] **Step 4:** Run `make test-quick` + `./scripts/check-helm-render.sh` → PASS.
- [ ] **Step 5: Commit** `git commit -am "feat(seo): canonical origin from site.base_url (prod-validated); sitemap Cache-Control"`

---

### Task 7: SSR post page — file template, data island, preview, allowlist

**Files:**
- Create: `templates/pages/blog_post.html`, `templates/pages/blog_post_404.html`
- Create: `src/pages/PageTemplates.hpp`
- Modify: `src/api/PublicPagesController.hpp` (rewrite `blogPost`), `src/utils/Strings.hpp` (`kDefaultPublicPathsCsv` + `/sitemap.xml,/blog/*`), `helm/tarassov-me/values.yaml` (`api.publicPaths` + same)
- Test: `tests/integration/test_post.cpp` (PublicPagesTest)

**Interfaces:**
- Produces: `Pages::render(const std::string& name, const std::map<std::string, std::string>& vars) -> std::string` — loads `templates/pages/<name>.html` once (static cache), replaces `{{KEY}}` occurrences with the (caller-pre-escaped) value. Config `site.pages_templates_dir`/`SITE_PAGES_TEMPLATES_DIR`, default `templates/pages` (mirrors `mail.templates_dir`, `src/email/Templates.hpp:45`).
- The page embeds `<script type="application/json" id="post-data">{POST_JSON}</script>` — consumed by Task 11 (`blog.js` hydrates without a second fetch).

- [ ] **Step 1: Failing tests**

```cpp
TEST_F(PublicPagesTest, BlogPostSsrHeadIsoDatesAndIsland) {
    json body = {{"slug", "ssr-post"}, {"title", "SSR & Co"}, {"summary", "sum"},
                 {"body", "# hello"}, {"status", "published"}, {"topic", "SRE"},
                 {"tags", {"sre"}}};
    auto created = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_EQ(created->statusCode(), k201Created);

    auto resp = call([&](auto cb) { pages.blogPost(TestHelpers::make_request(Get), std::move(cb), "ssr-post"); });
    ASSERT_EQ(resp->statusCode(), k200OK);
    const std::string html(resp->body());
    EXPECT_NE(html.find("<title>SSR &amp; Co — Michael Tarassov</title>"), std::string::npos);
    EXPECT_NE(html.find("<link rel=\"canonical\" href=\"https://example.test/blog/ssr-post\">"), std::string::npos);
    EXPECT_NE(html.find("og:url\" content=\"https://example.test/blog/ssr-post\""), std::string::npos);
    EXPECT_NE(html.find("id=\"post-data\""), std::string::npos);
    // JSON-LD dates are ISO 8601 (no space-separated PG form anywhere).
    EXPECT_EQ(html.find("+00\""), std::string::npos);
    EXPECT_NE(html.find("\"datePublished\":\"2"), std::string::npos);
    EXPECT_NE(html.find("T"), std::string::npos);
    // No noindex on published posts.
    EXPECT_EQ(html.find("noindex"), std::string::npos);
}

TEST_F(PublicPagesTest, BlogPostDraftPreviewIsNoindexed) {
    json body = {{"slug", "wip-page"}, {"title", "WIP"}, {"status", "draft"}};
    auto created = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    const std::string id = json::parse(std::string(created->body()))["data"]["id"];

    auto r404 = call([&](auto cb) { pages.blogPost(TestHelpers::make_request(Get), std::move(cb), "wip-page"); });
    EXPECT_EQ(r404->statusCode(), k404NotFound);

    const std::string tok = Security::Tokens::issue(Security::Auth::get().config().jwt_secret, id,
                                                    Security::Tokens::Purpose::Preview,
                                                    std::chrono::seconds(3600));
    auto req = TestHelpers::make_request(Get);
    req->setParameter("preview", tok);
    auto rOk = call([&](auto cb) { pages.blogPost(req, std::move(cb), "wip-page"); });
    EXPECT_EQ(rOk->statusCode(), k200OK);
    EXPECT_NE(std::string(rOk->body()).find("noindex"), std::string::npos);
}
```

- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.**

`src/pages/PageTemplates.hpp`:

```cpp
/**
 * @file PageTemplates.hpp
 * @brief File-based HTML page templates ({{KEY}} substitution). Mirrors the
 *        email template loader: files live in templates/pages/, are read once
 *        and cached. Values must be pre-escaped by the caller — the template
 *        layer does substitution only.
 */
#pragma once
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "utils/Config.hpp"

namespace Pages {

inline std::string templates_dir() {
    return Config::get().get<std::string>("site.pages_templates_dir", "SITE_PAGES_TEMPLATES_DIR",
                                          "templates/pages");
}

inline const std::string& load(const std::string& name) {
    static std::unordered_map<std::string, std::string> cache;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    std::ifstream f(templates_dir() + "/" + name + ".html");
    if (!f.good()) throw std::runtime_error("page template not found: " + name);
    std::stringstream ss;
    ss << f.rdbuf();
    return cache.emplace(name, ss.str()).first->second;
}

inline std::string render(const std::string& name, const std::map<std::string, std::string>& vars) {
    std::string out = load(name);
    for (const auto& [key, value] : vars) {
        const std::string needle = "{{" + key + "}}";
        for (std::size_t pos = 0; (pos = out.find(needle, pos)) != std::string::npos; pos += value.size())
            out.replace(pos, needle.size(), value);
    }
    return out;
}

}  // namespace Pages
```

`templates/pages/blog_post.html` — the exact markup currently concatenated in
`PublicPagesController.hpp:118-162` (head + shell), with placeholders:
`{{TITLE}} {{DESC}} {{CANONICAL}} {{OG_TITLE}} {{OG_IMAGE}} {{JSON_LD}} {{ROBOTS}}
{{TOPIC}} {{TOPIC_HIDDEN}} {{H1}} {{DEK}} {{DEK_HIDDEN}} {{POST_JSON}}` — plus the
island line right before the scripts:

```html
<script type="application/json" id="post-data">{{POST_JSON}}</script>
<script src="/js/marked.min.js"></script>
<script src="/js/blog.js"></script>
```

`{{ROBOTS}}` renders to `""` for published and
`<meta name="robots" content="noindex">\n` for previewed drafts.
`templates/pages/blog_post_404.html` — the current 404 body verbatim.

`blogPost` handler rewritten to: `resolve_post(slug, req->getParameter("preview"))`
(from Task 5 — move `resolve_post` into a shared header `src/api/HandlerSupport.hpp`
or duplicate-free location both controllers include; put it in
`PostsController.hpp` as a public static and call
`Api::PostsController::resolve_post` here), build the same JSON-LD as today
(dates now ISO automatically via Task 1), `esc()` each var, `POST_JSON` =
`json(post).dump()` with the existing `</`→`<\/` guard, respond with
`Pages::render("blog_post", vars)`. The 404 branch responds with
`Pages::render("blog_post_404", {})`, status 404. Delete `kHeadTail`, `kPostNav`
and the string-building block.

- [ ] **Step 4: Allowlist.** `src/utils/Strings.hpp` `kDefaultPublicPathsCsv` — append `,"/sitemap.xml,/blog/*"` (NOT `/blog-single.html` — it dies in Task 8). `helm/tarassov-me/values.yaml` `api.publicPaths` — append `,/sitemap.xml,/blog/*`.
- [ ] **Step 5:** Run `make test-quick`, `make lint-format`, `./scripts/check-helm-render.sh` → PASS. (Tests boot from repo root so `templates/pages/` resolves; Dockerfile already copies `/app/templates`.)
- [ ] **Step 6: Commit** `git commit -am "feat(seo): SSR post page from file templates + post-data island; preview renders noindex; allowlist sitemap+blog"`

---

### Task 8: Delete `/blog-single.html` everywhere

**Files:**
- Modify: `src/api/PublicPagesController.hpp` (drop `blogSingleRedirect` + its `ADD_METHOD_TO`), `src/api/Endpoints.hpp` (drop the entry), `docs/openapi.yaml` (drop the path)
- Modify: `frontend/nginx.conf` (drop the `location = /blog-single.html` block), `helm/tarassov-me-frontend/templates/configmap.yaml` (same block — keep the two files in sync)
- Delete: `frontend/public-site/blog-single.html`
- Test: `tests/integration/test_post.cpp` (update `SsrSitemapAndRedirect`: remove redirect assertions; assert sitemap no longer lists blog-single)

- [ ] **Step 1:** Remove code + files as listed. Registry, spec, nginx (both), static shell.
- [ ] **Step 2:** Update tests: delete the 301 assertions from `SsrSitemapAndRedirect` (rename it `SsrSitemap`); grep the test tree for `blog-single` — must be zero hits: `grep -rn "blog-single" tests/ src/ frontend/ helm/ docs/openapi.yaml` → only CHANGELOG/spec/plan mentions remain.
- [ ] **Step 3:** Run `make test-quick` + `./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh && ./scripts/check-helm-render.sh` → PASS.
- [ ] **Step 4: Commit** `git commit -am "feat(seo)!: remove legacy /blog-single.html (endpoint, nginx, static shell)"`

---

### Task 9: Admin list — q/status/topic/tag filters

**Files:**
- Modify: `src/repositories/PostRepository.hpp`, `src/api/PostsController.hpp` (`listPosts`), `docs/openapi.yaml`
- Test: `tests/integration/test_post.cpp`

**Interfaces:**
- Produces: `struct AdminListFilter { std::string q, status, topic, tag; }`; `list_admin(const AdminListFilter&, int limit, int offset) -> std::vector<Domain::Post>`; `count_admin(const AdminListFilter&) -> long`. Envelope unchanged: `{data,total,limit,offset}`.

- [ ] **Step 1: Failing test**

```cpp
TEST_F(PostsFlowTest, AdminListFilters) {
    seed("pub-k8s", "Kube Pub", "Kubernetes", {"kubernetes"});
    json draft = {{"slug", "draft-slo"}, {"title", "SLO Draft"}, {"status", "draft"}, {"topic", "SRE"}};
    call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, draft), std::move(cb)); });

    auto req = TestHelpers::make_request(Get);
    req->setParameter("status", "draft");
    auto resp = call([&](auto cb) { controller.listPosts(req, std::move(cb)); });
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"], 1);
    EXPECT_EQ(body["data"][0]["slug"], "draft-slo");

    auto req2 = TestHelpers::make_request(Get);
    req2->setParameter("q", "slo");
    auto r2 = call([&](auto cb) { controller.listPosts(req2, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(r2->body()))["total"], 1);
}
```

- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** Repo — same escaping approach as `public_where` (share a private `escape_like` helper), `q` covers `title+slug+summary`, no published-only constraint, `status`/`topic`/`tag` exact:

```cpp
    struct AdminListFilter { std::string q, status, topic, tag; };
private:
    template <typename Txn>
    static std::string admin_where(Txn& txn, const AdminListFilter& f) {
        std::string w = "TRUE";
        if (!f.status.empty()) w += " AND status = '" + txn.esc(f.status) + "'";
        if (!f.topic.empty())  w += " AND topic = '" + txn.esc(f.topic) + "'";
        if (!f.tag.empty())    w += " AND (',' || tags || ',') LIKE ('%,' || '" + txn.esc(f.tag) + "' || ',%')";
        if (!f.q.empty()) {
            std::string tmp;
            for (char c : f.q) { if (c=='%'||c=='_'||c=='\\') tmp += '\\'; tmp += c; }
            const std::string qq = txn.esc(tmp);
            w += " AND (title ILIKE '%" + qq + "%' OR slug ILIKE '%" + qq + "%' OR summary ILIKE '%" + qq + "%')";
        }
        return w;
    }
public:
    std::vector<Domain::Post> list_admin(const AdminListFilter& f, int limit, int offset) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec(std::string("SELECT ") + kColumns + " FROM posts WHERE " + admin_where(txn, f) +
                              " ORDER BY created_at DESC LIMIT " + std::to_string(limit) +
                              " OFFSET " + std::to_string(offset));
            std::vector<Domain::Post> out;
            out.reserve(r.size());
            for (const auto& row : r) out.push_back(Domain::Post::from_row(row));
            return out;
        });
    }
    long count_admin(const AdminListFilter& f) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec("SELECT count(*) FROM posts WHERE " + admin_where(txn, f));
            return r[0][0].template as<long>();
        });
    }
```

Controller `listPosts`: keep `parse_page_params(req, 50, 200)`; build
`AdminListFilter` from `q/status/topic/tag` params (validate `status` ∈
{"", "draft", "published"} → else 400 `bad_request("invalid_status", ...)`); call the
new methods; envelope unchanged.

- [ ] **Step 4:** openapi.yaml: add the four query params to `/api/v1/posts` GET. Run tests + drift → PASS.
- [ ] **Step 5: Commit** `git commit -am "feat(api): admin posts list — q/status/topic/tag filters"`

---

### Task 10: Media library — storage list/remove + endpoints

**Files:**
- Modify: `src/storage/Storage.hpp` (interface + LocalStorage + S3Storage `list`)
- Modify: `src/api/UploadController.hpp` (add `GET /api/v1/admin/uploads`, `DELETE /api/v1/admin/uploads/{1}`), `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/unit/test_storage_list.cpp` (create), `tests/integration/test_uploads_admin.cpp` (create)

**Interfaces:**
- Produces: `struct ObjectInfo { std::string key; std::size_t size_bytes; std::string last_modified; }` (ISO date or empty); `StorageBackend::list(const std::string& prefix) -> std::vector<ObjectInfo>` (pure virtual, implemented for both backends; S3 = single ListObjectsV2 page, `max-keys=1000` — log a warning if `IsTruncated` appears).
- `GET /api/v1/admin/uploads?limit&offset` → `{data:[{key,name,url,size_bytes,content_type,created_at}],total,limit,offset}` (admin envelope style; `name` = basename, the DELETE address). `DELETE /api/v1/admin/uploads/{name}` — single URL-safe segment; the server resolves `posts/<name>` (upload keys are `posts/<hex>.<ext>`, `UploadController.hpp:108`); 400 on `/` or unsafe chars, 404 when absent.

- [ ] **Step 1: Failing unit test**

```cpp
// tests/unit/test_storage_list.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include "storage/Storage.hpp"

TEST(StorageList, LocalListsPrefixOnly) {
    const auto root = std::filesystem::temp_directory_path() / "storage-list-test";
    std::filesystem::remove_all(root);
    Storage::LocalStorage s(root, "http://cdn.test");
    s.put("posts/aaa.png", "x", "image/png");
    s.put("posts/bbb.jpg", "yy", "image/jpeg");
    s.put("other/ccc.png", "z", "image/png");
    auto items = s.list("posts/");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].key.rfind("posts/", 0), 0u);
    EXPECT_GT(items[1].size_bytes, 0u);
    std::filesystem::remove_all(root);
}
```

- [ ] **Step 2: Run** `make test-unit` → FAIL. **Step 3: Implement storage.**

`StorageBackend`: add

```cpp
    struct ObjectInfo {
        std::string key;
        std::size_t size_bytes = 0;
        std::string last_modified;  // ISO 8601 or empty when unknown
    };
    virtual std::vector<ObjectInfo> list(const std::string& prefix) = 0;
```

`LocalStorage::list`:

```cpp
    std::vector<ObjectInfo> list(const std::string& prefix) override {
        std::vector<ObjectInfo> out;
        if (!key_is_safe(prefix.empty() ? "x" : prefix))
            throw std::runtime_error("storage: unsafe prefix");
        std::error_code ec;
        const auto base = root_ / prefix;
        if (!std::filesystem::exists(base, ec)) return out;
        for (auto it = std::filesystem::recursive_directory_iterator(base, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file(ec)) continue;
            ObjectInfo o;
            o.key = prefix + std::filesystem::relative(it->path(), base, ec).generic_string();
            o.size_bytes = static_cast<std::size_t>(it->file_size(ec));
            const auto ft = std::filesystem::last_write_time(*it, ec);
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  ft.time_since_epoch() - std::filesystem::file_time_type::clock::now().time_since_epoch() +
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
            o.last_modified = Utils::Time::epoch_to_iso8601(secs);
            out.push_back(std::move(o));
        }
        std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.last_modified > b.last_modified; });
        return out;
    }
```

(`#include "utils/Time.hpp"`, `<algorithm>`, `<chrono>`.) `S3Storage::list`: GET
`/?list-type=2&prefix=<enc>` (extend `request()` with an optional
query-string parameter that participates in SigV4 canonical query), then scan
the XML body for `<Contents>` blocks extracting `<Key>`, `<Size>`,
`<LastModified>` via `find` — no XML library:

```cpp
    std::vector<ObjectInfo> list(const std::string& prefix) override {
        std::string body;
        const long code = request_list(prefix, &body);
        if (code != 200) throw std::runtime_error("s3: LIST failed with HTTP " + std::to_string(code));
        std::vector<ObjectInfo> out;
        std::size_t pos = 0;
        while ((pos = body.find("<Contents>", pos)) != std::string::npos) {
            const auto end = body.find("</Contents>", pos);
            auto grab = [&](const char* tag) -> std::string {
                const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
                const auto a = body.find(open, pos);
                if (a == std::string::npos || a > end) return {};
                const auto b = body.find(close, a);
                return body.substr(a + open.size(), b - a - open.size());
            };
            ObjectInfo o;
            o.key = grab("Key");
            const std::string sz = grab("Size");
            o.size_bytes = sz.empty() ? 0 : static_cast<std::size_t>(std::stoull(sz));
            std::string lm = grab("LastModified");           // 2026-07-25T05:34:32.000Z
            if (lm.size() > 20) lm = lm.substr(0, 19) + "Z";  // drop millis
            o.last_modified = lm;
            if (!o.key.empty()) out.push_back(std::move(o));
            pos = end;
        }
        if (body.find("<IsTruncated>true</IsTruncated>") != std::string::npos)
            spdlog::warn("s3 list: >1000 objects under '{}' — listing truncated", prefix);
        return out;
    }
```

`request_list` mirrors `request()` with empty key, canonical URI `/<bucket>`,
canonical query `list-type=2&prefix=<uri-encoded>` (alphabetical order —
`list-type` < `prefix`), same signing. (Copy `request()`, replace the URI/query
lines; ~30 lines.)

- [ ] **Step 4: Endpoints.** In `UploadController.hpp` register + implement:

```cpp
    ADD_METHOD_TO(UploadController::listUploads, "/api/v1/admin/uploads", Get);
    ADD_METHOD_TO(UploadController::deleteUpload, "/api/v1/admin/uploads/{1}", Delete);

    static std::string content_type_for(const std::string& name) {
        const auto dot = name.rfind('.');
        std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
        if (ext == "png") return "image/png";
        if (ext == "gif") return "image/gif";
        if (ext == "webp") return "image/webp";
        return "application/octet-stream";
    }

    void listUploads(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        if (!Storage::is_initialized()) {
            callback(ErrorResponse::service_unavailable("storage_unavailable", "Storage backend not configured"));
            return;
        }
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);
        auto all = Storage::get().list("posts/");
        json data = json::array();
        const std::size_t from = std::min<std::size_t>(page.offset, all.size());
        const std::size_t to = std::min<std::size_t>(from + page.limit, all.size());
        for (std::size_t i = from; i < to; ++i) {
            const auto& o = all[i];
            const std::string name = o.key.substr(o.key.rfind('/') + 1);
            data.push_back({{"key", o.key}, {"name", name}, {"url", Storage::get().url(o.key)},
                            {"size_bytes", o.size_bytes}, {"content_type", content_type_for(name)},
                            {"created_at", o.last_modified}});
        }
        callback(Response::ok({{"data", data}, {"total", static_cast<long>(all.size())},
                               {"limit", page.limit}, {"offset", page.offset}}));
    }

    void deleteUpload(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& name) {
        API_REQUIRE_ADMIN(req, callback);
        // One URL-safe segment: the basename of an upload key (posts/<name>).
        if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) {
            callback(ErrorResponse::bad_request("invalid_name", "Expected a single-segment object name"));
            return;
        }
        if (!Storage::is_initialized()) {
            callback(ErrorResponse::service_unavailable("storage_unavailable", "Storage backend not configured"));
            return;
        }
        const std::string key = "posts/" + name;
        if (!Storage::get().exists(key)) {
            callback(ErrorResponse::not_found("upload"));
            return;
        }
        Storage::get().remove(key);
        callback(Response::ok({{"message", "Upload deleted"}}));
    }
```

(`#include "api/RequestUtils.hpp"` if not present.) Integration test
`tests/integration/test_uploads_admin.cpp` — CoreBackedTest fixture, install a
temp LocalStorage via `Storage::install_for_testing`, put two objects, assert
list envelope + delete → gone + second delete → 404 + `DELETE a/../b` shape is
unreachable (route param can't contain `/`; assert the `..` guard with name
`"..evil"` → 400 by the guard? `..` check catches it → expect 400).

- [ ] **Step 5:** Endpoints.hpp entries: `{"GET", "/api/v1/admin/uploads", "Admin: list uploaded images"}`, `{"DELETE", "/api/v1/admin/uploads/{name}", "Admin: delete an uploaded image"}`. openapi.yaml paths. Run `make test-quick` + drift scripts → PASS.
- [ ] **Step 6: Commit** `git commit -am "feat(api): media library — storage list() + admin uploads list/delete"`

---

### Task 11: Public frontend — blog.js on the new contract

**Files:**
- Modify: `frontend/public-site/js/blog.js`
- Verify shells: `frontend/public-site/blog.html` (ids unchanged), `templates/pages/blog_post.html` (island id `post-data`)

**Interfaces:**
- Consumes: `{items,page,limit,total,facets:{topics:[{name,count}],tags:[{name,count}]}}`; by-slug `data.adjacent.{prev,next}`; island `#post-data` = the by-slug `data` object (post JSON).
- Behavior change (spec §3.1, single `topic`/`tag` params): chips become single-select — a click replaces the active filter; clicking the active chip (or All) clears it. Server does filtering/paging; TanStack-free page — rely on HTTP cache semantics only.

- [ ] **Step 1: Rewrite `renderList()`** (replace the whole function):

```js
    function renderList() {
        var listEl = document.getElementById("blog-posts");
        if (!listEl) return;
        var filterEl = document.getElementById("blog-filter");
        var topicsEl = document.getElementById("blog-topics");
        var keywordsEl = document.getElementById("blog-keywords");
        var keywordRowEl = document.getElementById("blog-keyword-row");
        var resultEl = document.getElementById("blog-result");
        var pagerEl = document.getElementById("blog-pager");

        var PER_PAGE = 10;
        // active: {kind:"topic"|"tag", name} | null. Single-select — the API
        // filters server-side (?topic= / ?tag=), one request per interaction.
        var state = { active: null, page: 1, grandTotal: null };

        function load() {
            var qs = "?page=" + state.page + "&limit=" + PER_PAGE + "&include=facets";
            if (state.active) qs += "&" + state.active.kind + "=" + encodeURIComponent(state.active.name);
            fetch(API + qs)
                .then(function (r) { return r.json(); })
                .then(function (res) {
                    if (state.grandTotal === null && !state.active) state.grandTotal = res.total;
                    draw(res);
                })
                .catch(function () {
                    listEl.innerHTML = '<p class="blog-loading">Failed to load posts.</p>';
                });
        }

        function setFilter(kind, name) {
            var same = state.active && state.active.kind === kind && state.active.name === name;
            state.active = same ? null : { kind: kind, name: name };
            state.page = 1;
            load();
        }

        function chip(name, kind) {
            var b = document.createElement("button");
            b.type = "button";
            var on = state.active && state.active.kind === kind && state.active.name === name;
            b.className = "blog-chip-btn" + (kind === "tag" ? " is-dim" : "") + (on ? " is-active" : "");
            b.textContent = name;
            b.addEventListener("click", function () { setFilter(kind, name); });
            return b;
        }

        function drawTopics(facets) {
            if (!topicsEl) return;
            topicsEl.innerHTML = "";
            var all = document.createElement("button");
            all.type = "button";
            all.className = "blog-chip-btn" + (state.active ? "" : " is-active");
            all.textContent = "All";
            all.addEventListener("click", function () { state.active = null; state.page = 1; load(); });
            topicsEl.appendChild(all);
            (facets.topics || []).forEach(function (t) { topicsEl.appendChild(chip(t.name, "topic")); });
        }

        function drawKeywords(facets) {
            if (!keywordsEl || !keywordRowEl) return;
            var names = (facets.tags || []).slice(0, 14);
            if (names.length <= 1) { keywordRowEl.setAttribute("hidden", ""); return; }
            keywordsEl.innerHTML = "";
            names.forEach(function (t) { keywordsEl.appendChild(chip(t.name, "tag")); });
            keywordRowEl.removeAttribute("hidden");
        }

        function pad2(n) { return String(n).padStart(2, "0"); }

        function draw(res) {
            if (filterEl) filterEl.removeAttribute("hidden");
            var facets = res.facets || { topics: [], tags: [] };
            drawTopics(facets);
            drawKeywords(facets);

            if (resultEl) {
                resultEl.textContent = state.active
                    ? res.total + " / " + (state.grandTotal === null ? res.total : state.grandTotal)
                    : res.total + " ARTICLES";
            }

            var items = res.items || [];
            if (!items.length) {
                listEl.innerHTML = '<p class="blog-empty">' +
                    (state.active ? "No articles match this filter." : "No posts yet.") + "</p>";
            } else {
                listEl.innerHTML = items.map(function (p) {
                    var chips = postTags(p).map(function (t) {
                        var on = state.active && state.active.kind === "tag" && state.active.name === t;
                        return '<span class="blog-chip' + (on ? " is-active" : "") + '">' + esc(t) + "</span>";
                    }).join("");
                    return (
                        '<a class="blog-row" href="/blog/' + encodeURIComponent(p.slug) + '">' +
                        '<div class="blog-row-meta"><div>' + esc(fmtDate(p.published_at)) + "</div>" +
                        '<div class="blog-row-read">' + (p.read_mins || 1) + " MIN</div></div>" +
                        "<div>" +
                        (p.topic ? '<div class="blog-row-topic">' + esc(p.topic) + "</div>" : "") +
                        '<h3 class="blog-row-title">' + esc(p.title) + "</h3>" +
                        (chips ? '<div class="blog-chips">' + chips + "</div>" : "") +
                        "</div></a>");
                }).join("");
            }
            drawPager(res.total, Math.max(1, Math.ceil(res.total / PER_PAGE)), state.page - 1);
        }

        function drawPager(count, totalPages, page) {
            if (!pagerEl) return;
            if (totalPages <= 1) {
                pagerEl.innerHTML = "";
                pagerEl.removeAttribute("class");
                pagerEl.setAttribute("hidden", "");
                return;
            }
            var from = page * PER_PAGE + 1;
            var to = Math.min(count, page * PER_PAGE + PER_PAGE);
            pagerEl.className = "blog-pager";
            pagerEl.innerHTML = "";
            var range = document.createElement("div");
            range.className = "blog-range";
            range.textContent = count ? from + "–" + to + " OF " + count : "NOTHING HERE";
            pagerEl.appendChild(range);
            var nav = document.createElement("div");
            nav.className = "blog-pager-nav";
            var prev = document.createElement("button");
            prev.type = "button"; prev.className = "blog-arrow"; prev.textContent = "←";
            prev.disabled = page <= 0;
            prev.addEventListener("click", function () { state.page = page; load(); });
            nav.appendChild(prev);
            var counter = document.createElement("span");
            counter.className = "blog-counter";
            counter.textContent = pad2(page + 1) + " / " + pad2(totalPages);
            nav.appendChild(counter);
            var next = document.createElement("button");
            next.type = "button"; next.className = "blog-arrow"; next.textContent = "→";
            next.disabled = page >= totalPages - 1;
            next.addEventListener("click", function () { state.page = page + 2; load(); });
            nav.appendChild(next);
            pagerEl.appendChild(nav);
            pagerEl.removeAttribute("hidden");
        }

        load();
    }
```

- [ ] **Step 2: Rewrite `renderSingle()` data source + slug parsing.** Replace the `?slug=` fallback and the fetch with island-first:

```js
        // Clean URL only: /blog/<slug> (the legacy ?slug= shell is gone).
        var pathMatch = location.pathname.match(/^\/blog\/(.+)$/);
        var slug = pathMatch ? decodeURIComponent(pathMatch[1]) : null;
        if (!slug) { fail("No post specified."); return; }

        // SSR island first — the server embeds the post JSON so no second
        // request is needed; fall back to the API if the island is missing.
        var island = document.getElementById("post-data");
        var islandPost = null;
        if (island) {
            try { islandPost = JSON.parse(island.textContent); } catch (e) { islandPost = null; }
        }
        var preview = new URLSearchParams(location.search).get("preview");
        var api = API + "/" + encodeURIComponent(slug) +
                  "?include=adjacent" + (preview ? "&preview=" + encodeURIComponent(preview) : "");
        (islandPost
            ? Promise.resolve({ data: islandPost })
            : fetch(api).then(function (r) { if (!r.ok) throw new Error("not found"); return r.json(); })
        ).then(function (res) { /* existing rendering body, unchanged */ })
```

and replace `renderPager(slug)` (the whole feed refetch) with adjacent-driven:

```js
    // Prev/next from the by-slug include=adjacent payload (or a 1-shot fetch
    // when we hydrated from the island, which carries no adjacent block).
    function renderPager(slug, adjacent) {
        var pager = document.getElementById("post-pager");
        if (!pager) return;
        var apply = function (adj) {
            if (!adj) return;
            function side(id, ref) {
                var a = document.getElementById(id);
                if (!a || !ref) return false;
                a.href = "/blog/" + encodeURIComponent(ref.slug);
                a.querySelector(".title").textContent = ref.title;
                a.removeAttribute("hidden");
                return true;
            }
            var hasOlder = side("pager-prev", adj.prev);
            var hasNewer = side("pager-next", adj.next);
            if (hasOlder || hasNewer) pager.removeAttribute("hidden");
            bindProgress();
        };
        if (adjacent) { apply(adjacent); return; }
        fetch(API + "/" + encodeURIComponent(slug) + "?include=adjacent")
            .then(function (r) { return r.json(); })
            .then(function (res) { apply(res && res.data && res.data.adjacent); })
            .catch(function () {});
    }
```

Call it `renderPager(slug, post.adjacent)` from the render body.
`renderLatest()`: change envelope `res.data` → `res.items` and the query to
`?limit=2` (params otherwise unchanged). Update the file header comment (routes list).

- [ ] **Step 3: Manual smoke via compose:** `make up-build` (app+frontend), then `curl -s localhost:8081/blog.html >/dev/null && curl -s "localhost:8080/api/v1/public/posts?include=facets" | head -c 300` — envelope visible; open `/blog.html` if a browser is available. Down: `make down`.
- [ ] **Step 4: Commit** `git commit -am "feat(blog)!: index on server filters/facets/paging; single page hydrates from SSR island; adjacent from API"`

---

### Task 12: Admin frontend — search/filter, preview, media page

**Files:**
- Modify: `frontend/src/pages/admin/Posts.tsx`, `frontend/src/lib/api/queryKeys.ts`
- Create: `frontend/src/pages/admin/Media.tsx`
- Modify: the admin route manifest (locate: `grep -rn "admin/posts\|AdminPostsPage" frontend/src/routes/`) — add the media route next to the posts entry, same guard/permission
- Regenerate: `frontend/src/lib/api/schema.gen.ts`

- [ ] **Step 1: queryKeys** — extend `qk.admin`:

```ts
    posts: (filter?: string, page?: number) => {
      if (filter === undefined) return ['admin', 'posts'] as const;
      if (page === undefined) return ['admin', 'posts', filter] as const;
      return ['admin', 'posts', filter, page] as const;
    },
    media: () => ['admin', 'media'] as const,
```

(existing `qk.admin.posts()` call sites keep working — invalidation by prefix.)

- [ ] **Step 2: Posts.tsx** — add filter state + controls above the table and a Preview action:

```tsx
  const [q, setQ] = useState('');
  const [status, setStatus] = useState<'' | 'draft' | 'published'>('');
  const [qDebounced, setQDebounced] = useState('');
  useEffect(() => {
    const t = setTimeout(() => setQDebounced(q), 300);
    return () => clearTimeout(t);
  }, [q]);

  const filterKey = JSON.stringify({ q: qDebounced, status });
  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.admin.posts(filterKey),
    queryFn: ({ limit, offset }) =>
      api.getJson<{ data: Post[]; total: number }>('/api/v1/posts', {
        query: { limit, offset, ...(qDebounced ? { q: qDebounced } : {}), ...(status ? { status } : {}) },
      }),
    perPage: PER_PAGE,
  });

  const preview = useApiMutation(
    (id: string) =>
      api.postJson<{ data: { url: string; expires_at: string } }>(`/api/v1/posts/${id}/preview-token`, {}),
    { onSuccess: (res) => window.open(res.data.url, '_blank', 'noopener') },
  );
```

(import `useEffect`; `usePagedQuery` resets its page when the query key changes —
verify, else `useEffect(() => setPage(1), [filterKey])`.) Controls JSX between the
header and the Card:

```tsx
      <div className="flex gap-2">
        <Input placeholder="Search title, slug, summary…" value={q} onChange={(e) => setQ(e.target.value)} />
        <select
          className="h-10 rounded-md border border-input bg-background px-3 text-sm"
          value={status}
          onChange={(e) => setStatus(e.target.value as '' | 'draft' | 'published')}
        >
          <option value="">all</option>
          <option value="draft">draft</option>
          <option value="published">published</option>
        </select>
      </div>
```

Actions column: replace the stale `blog-single.html` link with the clean URL and
add draft preview:

```tsx
          {p.status === 'published' ? (
            <Button asChild size="sm" variant="ghost" title="View on the public site">
              <a href={`/blog/${encodeURIComponent(p.slug)}`} target="_blank" rel="noopener">
                <ExternalLink className="h-3.5 w-3.5" />
              </a>
            </Button>
          ) : (
            <Button size="sm" variant="ghost" title="Preview draft"
                    disabled={preview.isPending}
                    onClick={() => preview.mutate(p.id)}>
              <Eye className="h-3.5 w-3.5" />
            </Button>
          )}
```

(import `Eye` from `lucide-react`; add `preview.error` to `useErrorToast`.)

- [ ] **Step 3: Media.tsx** — new page mirroring Posts.tsx structure:

```tsx
import { useState } from 'react';
import { Link } from 'react-router-dom';
import { Trash2 } from 'lucide-react';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

const PER_PAGE = 50;

interface UploadItem {
  key: string;
  name: string;
  url: string;
  size_bytes: number;
  content_type: string;
  created_at: string;
}

function fmtSize(n: number): string {
  if (n >= 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(0) + ' KB';
  return n + ' B';
}

export function AdminMediaPage() {
  const [deleting, setDeleting] = useState<UploadItem | null>(null);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.admin.media(),
    queryFn: ({ limit, offset }) =>
      api.getJson<{ data: UploadItem[]; total: number }>('/api/v1/admin/uploads', {
        query: { limit, offset },
      }),
    perPage: PER_PAGE,
  });

  const remove = useApiMutation(
    (name: string) => api.deleteJson(`/api/v1/admin/uploads/${encodeURIComponent(name)}`),
    { invalidate: [qk.admin.media()], onSuccess: () => setDeleting(null) },
  );
  useErrorToast(remove.error);

  const columns: Column<UploadItem>[] = [
    {
      header: '',
      className: 'w-16',
      cell: (u) => <img src={u.url} alt={u.name} className="h-10 w-10 rounded object-cover" loading="lazy" />,
    },
    { header: 'Name', className: 'font-mono text-xs', cell: (u) => u.name },
    { header: 'Size', className: 'text-xs', cell: (u) => fmtSize(u.size_bytes) },
    { header: 'Uploaded', className: 'text-xs', cell: (u) => u.created_at.slice(0, 10) },
    {
      header: '',
      className: 'text-right',
      cell: (u) => (
        <Button size="sm" variant="ghost" onClick={() => setDeleting(u)}>
          <Trash2 className="h-3.5 w-3.5 text-destructive" />
        </Button>
      ),
    },
  ];

  return (
    <div className="container mx-auto max-w-4xl py-8 space-y-6">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-3xl font-bold">Media</h1>
          <p className="text-sm text-muted-foreground">Images uploaded from the post editor.</p>
        </div>
        <Button asChild variant="ghost">
          <Link to="/admin">← Admin</Link>
        </Button>
      </div>
      <Card>
        <CardHeader>
          <CardTitle>{data ? `${data.total} file(s)` : 'Loading…'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(u) => u.key}
            isLoading={isLoading}
            error={error}
            emptyText="No uploads yet."
            isPlaceholder={isPlaceholderData}
          />
          {data && (
            <PaginationFooter page={page} totalPages={totalPages}
                              isPlaceholderData={isPlaceholderData} onPageChange={setPage} />
          )}
        </CardContent>
      </Card>
      {deleting && (
        <ConfirmDialog
          title="Delete file"
          description={`Delete "${deleting.name}"? Posts that embed it will show a broken image.`}
          confirmLabel="Delete file"
          destructive
          busy={remove.isPending}
          onConfirm={() => remove.mutate(deleting.name)}
          onClose={() => setDeleting(null)}
        />
      )}
    </div>
  );
}
```

Route: add `/admin/media` beside the posts entry in the manifest (same admin
guard), and a "Media" link on the admin dashboard if a nav list exists (`grep -rn
"Posts" frontend/src/pages/admin/Dashboard.tsx`).

- [ ] **Step 4: Regenerate + full frontend gate** (docker command from Global Constraints; includes `gen:api`, typecheck, tests, lint, build) → all green. The regenerated `schema.gen.ts` reflects every openapi change from Tasks 3–10.
- [ ] **Step 5: Commit** `git commit -am "feat(admin): posts search/status filter, draft preview button, media library page"`

---

### Task 13: Contract close-out — docs, changelog, full gate

**Files:**
- Modify: `CHANGELOG.md` (`## [Unreleased]`), `docs/CONFIG.md` (verify site.base_url row from Task 6), `README.md` (blog API bullet: client render note stays, new endpoints list)
- Verify-only: full CI-equivalent run

- [ ] **Step 1: CHANGELOG entry** under `## [Unreleased]`:

```markdown
### Changed
- **Blog API contract v2 (breaking, in place).** Public list is `{items,page,limit,total}`
  with server-side `topic/tag/q` filters, 1-based `page`, `limit` ≤ 50 and
  `include=facets`; by-slug gains `include=adjacent` and `?preview=`. Admin list
  gains `q/status/topic/tag`. All API timestamps are ISO 8601 UTC.
- **SEO surface rebuilt.** Absolute URLs derive from `site.base_url`
  (SITE_BASE_URL, required https in prod); SSR post pages render from
  `templates/pages/` and embed a `#post-data` island (no double fetch);
  sitemap sends `Cache-Control: max-age=3600`.
- **Index filter is single-select** (one topic OR one tag), server-filtered.

### Added
- Draft preview: `POST /api/v1/posts/{id}/preview-token` → 1-hour signed link;
  `/blog/<slug>?preview=` renders the draft with `noindex`.
- Media library: `GET/DELETE /api/v1/admin/uploads` + admin Media page.
- Migration 008: numeric-slug posts get `topic='LeetCode'` (derivation removed from JS).

### Removed
- `/blog-single.html` (endpoint, nginx locations, static shell, `?slug=` fallback).
  Old links now 404 — accepted risk (spec 2026-07-25).
```

- [ ] **Step 2: Full local gate, in order:**

```bash
make lint-format
./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh && ./scripts/check-test-buckets.sh
./scripts/check-helm-render.sh
make test
# frontend (docker node:20 command from Global Constraints)
git status --short   # must be clean except intended changes
```

All green, `git diff origin/main --stat` reviewed — no stray files.

- [ ] **Step 3: Commit** `git commit -am "docs: changelog + config docs for blog API contract v2"`
- [ ] **Step 4:** Deployment note for the PR body (do NOT deploy in this task): prod `api` release carries a live `api.publicPaths` override — it must gain `/sitemap.xml,/blog/*` (and drop nothing else); `SITE_BASE_URL=https://tarassov.me` must be set or the pod refuses to start; frontend configmap loses the blog-single location. PR #5 is superseded — close it referencing this PR.

---

## Self-Review Notes

- Spec coverage: §3.1 → Tasks 3–4, §3.2 → Tasks 6–8, §3.3 → Tasks 5, 9, 10, 12, §3.4 → Tasks 1–2, §5 приёмка → per-task drift/allowlist steps + Task 13 gate. Deletion checklist §5.5 → Task 8 + blog.js fallback in Task 11.
- Envelope consistency: public `{items,...}` (Tasks 3, 11), admin `{data,...}` (Tasks 9, 10, 12) — intentional split per spec.
- `resolve_post` defined in Task 5, reused in Task 7 (referenced by exact name).
- Facets `tags` capped at 30 server-side; UI shows 14 (Task 11 slice) — spec's "top tags of the current result".
