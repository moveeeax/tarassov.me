# Tabler dashboard shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Превратить `/admin` в каркас личного дашборда: настраиваемая главная с сеткой виджетов на бэкенд-API плюс перевод всей админки с Tailwind и shadcn на Tabler.

**Architecture:** Раскладка это owner-scoped ресурс по конвенциям репозитория (миграция, DTO, `CrudBase` с `kOwnerColumn`, контроллер, OpenAPI). Каталог типов виджетов живет `constexpr`-массивом в C++ и оттуда же валидирует запись, фронт берет его запросом и не держит второй список. Дизайн-система меняется целиком, но Tailwind удаляется последней задачей, чтобы каждый промежуточный коммит оставался читаемым.

**Tech Stack:** C++20, Drogon, libpqxx, PostgreSQL 15, nlohmann::json, gtest. React 18, TypeScript 5.6, Vite 5, TanStack Query 5, react-hook-form, zod, vitest. `@tabler/core` 1.6.0, `@tabler/icons-react` 3.48.0, `apexcharts` с `react-apexcharts` 2.1.1, `@fontsource-variable/inter` 5.3.0, `react-grid-layout` 2.2.4.

**Spec:** `docs/superpowers/specs/2026-09-28-tabler-dashboard-shell-design.md`

## Global Constraints

- Дизайн-система только Tabler. Пакет `@tabler/core@1.6.0` из npm, форк `moveeeax/tabler` не вендорится (проверено 28.09.2026: совпадает с `tabler/tabler@dev`, ahead 0, behind 0).
- Bootstrap JS не подключается. Поведение (модалки, дропдауны, коллапс навигации, фокус-трап) на своих React-компонентах.
- CSP админки не ослабляется: `script-src 'self'`, `style-src 'self' 'unsafe-inline'`, `font-src 'self'`, `img-src 'self' data:`. Шрифт ставится пакетом и уходит в бандл.
- Не трогаются: `frontend/public-site/`, `templates/pages/`, `frontend/nginx.conf`, `helm/`, публичный контракт `GET /api/v1/public/posts`.
- Все модули под `src/` заголовочные. Новый `.cpp` там не линкуется вообще.
- JSON только `nlohmann::json`. `req->getJsonObject()` не вызывается.
- `callback(...)` вызывается ровно один раз на каждом пути, включая ранние возвраты и раскрытия гардов.
- Лямбда в `execute_write` принимает `auto& txn` (внутрь приходит `detail::TracingTxn&`, не `pqxx::work&`).
- На таблице с владельцем только `find_owned` / `list_owned` / `count_owned`. Плоский `find`/`list` это IDOR.
- Сетка 12 колонок. Не больше 40 виджетов в наборе. `grid_x` 0..11, `grid_w` от `min_w` до 12, `grid_x + grid_w` не больше 12, `grid_y` 0..200, `grid_h` от `min_h` до 24.
- Миграции без `BEGIN`/`COMMIT`, DDL идемпотентный (`IF NOT EXISTS`).
- Роут добавляется и в `ADD_METHOD_TO`, и в `Api::get_endpoints()` в `src/api/Endpoints.hpp`, и в `docs/openapi.yaml`.
- Коммиты обычные, по conventional commits, на английском. Трейлеры про AI-авторство не добавляются.

## Review Focus

Пять случаев, которые спек подразумевает, но которые легко оставить без теста. Для каждого ниже указана задача, в которую добавлен тест.

1. Пустой `widgets: []` в `PUT` очищает раскладку, а не отвечает 400 (задача 3).
2. Нецелые и отрицательные значения сетки (`grid_x: 1.5`, `grid_y: -1`) дают 400, а не 500 от `type_error.302` (задачи 1 и 3).
3. Известный ключ `options` со значением вне границ (`limit: 999`) дает 400 (задачи 1 и 3).
4. Два виджета одного типа в одном наборе разрешены: у них разные `options` (задача 3).
5. `localStorage` недоступен или бросает: тема и страница работают, раскладка не теряется (задача 4).

## File Structure

Бэкенд:

| Файл | Ответственность |
|---|---|
| `migrations/010_dashboard_widgets.sql` | Таблица раскладки, индекс, триггер `updated_at` |
| `src/domain/WidgetCatalog.hpp` | Каталог типов виджетов и валидация геометрии и опций. Чистая логика, без HTTP и SQL |
| `src/domain/DashboardWidget.hpp` | DTO строки: поля, `from_row`, `to_json` |
| `src/repositories/DashboardWidgetRepository.hpp` | Доступ к таблице: `CrudBase` плюс `replace_all` |
| `src/api/DashboardController.hpp` | Три ручки, гарды, валидация, маппинг ошибок |
| `src/api/Endpoints.hpp` | Реестр роутов (правка) |
| `src/api/Api.hpp` | Включение контроллера (правка) |
| `docs/openapi.yaml` | Контракт и схема `DashboardWidget` (правка) |
| `tests/unit/test_widget_catalog.cpp` | Каталог и валидация без базы |
| `tests/integration/test_dashboard_layout.cpp` | Репозиторий и ручки на живом Postgres |

Фронт:

| Файл | Ответственность |
|---|---|
| `frontend/src/components/tabler/*` | Примитивы на классах Tabler вместо `components/ui/*` |
| `frontend/src/components/Layout.tsx` | Оболочка `page` / `page-wrapper` / `page-body` |
| `frontend/src/components/Nav.tsx` | Боковая `navbar-vertical`, свой коллапс и переключатель темы |
| `frontend/public/theme.js` | Тема до первой отрисовки через `data-bs-theme` |
| `frontend/src/lib/api/dashboard.ts` | Типы и вызовы раскладки и каталога |
| `frontend/src/hooks/useDashboardLayout.ts` | Чтение и сохранение раскладки, дебаунс |
| `frontend/src/widgets/registry.tsx` | Тип виджета в React-компонент |
| `frontend/src/widgets/*.tsx` | Пять виджетов первой версии |
| `frontend/src/pages/admin/Dashboard.tsx` | Главная с сеткой и режимом настройки |

---

### Task 1: Каталог виджетов и валидация

Чистая логика без базы и HTTP, поэтому идет первой: на ней стоит валидация контроллера и от нее зависит фронтовый реестр.

**Files:**
- Create: `src/domain/WidgetCatalog.hpp`
- Test: `tests/unit/test_widget_catalog.cpp`

**Interfaces:**
- Consumes: `Api::Validation::Errors` (`src/api/Validation.hpp`), `Domain::Permission::kAdminister` и `kAuditRead` (`src/domain/Role.hpp`).
- Produces:
  - `Domain::Widgets::kGridColumns` (`int`, 12), `kMaxGridY` (200), `kMaxGridH` (24), `kMaxWidgets` (`std::size_t`, 40)
  - `struct Domain::Widgets::OptionSpec { std::string_view key; long long min_value, max_value, default_value; }`
  - `struct Domain::Widgets::WidgetSpec { std::string_view type, title, description; std::uint32_t required_permission; int default_w, default_h, min_w, min_h; const OptionSpec* options; std::size_t option_count; }`
  - `const WidgetSpec* Domain::Widgets::find(std::string_view type)`
  - `template <typename PermitFn> nlohmann::json Domain::Widgets::catalog_json(PermitFn&& permitted)`
  - `void Domain::Widgets::validate_widget(Api::Validation::Errors&, const nlohmann::json& widget, const WidgetSpec&, std::size_t index)`

- [ ] **Step 1: Write the failing test**

Создать `tests/unit/test_widget_catalog.cpp`:

```cpp
/**
 * @file test_widget_catalog.cpp
 * @brief The widget catalog is the single list the API validates against and
 *        publishes. Pure logic, no Postgres — runs in the unit bucket.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/Validation.hpp"
#include "domain/Role.hpp"
#include "domain/WidgetCatalog.hpp"

using json = nlohmann::json;
namespace W = Domain::Widgets;

namespace {

json valid_widget(const char* type = "posts_summary") {
    return json{{"widget_type", type}, {"grid_x", 0}, {"grid_y", 0}, {"grid_w", 4}, {"grid_h", 3}};
}

// Every catalog entry an admin may place, as the controller's own filter sees it.
bool admin_permits(std::uint32_t /*perm*/) {
    return true;
}

TEST(WidgetCatalog, FindsKnownTypeAndRejectsUnknown) {
    const auto* spec = W::find("posts_summary");
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->min_w, 3);
    EXPECT_EQ(W::find("no_such_widget"), nullptr);
}

TEST(WidgetCatalog, CatalogJsonFiltersByPermission) {
    // A caller holding only kAuditRead sees exactly the audit widget.
    auto only_audit = [](std::uint32_t perm) { return perm == Domain::Permission::kAuditRead; };
    auto filtered = W::catalog_json(only_audit);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0]["type"], "audit_recent");

    auto all = W::catalog_json(admin_permits);
    EXPECT_EQ(all.size(), 5u);
    EXPECT_TRUE(all[0].contains("default_w"));
    EXPECT_TRUE(all[0]["options"].is_object());
}

TEST(WidgetCatalog, AcceptsValidWidget) {
    Api::Validation::Errors errs;
    W::validate_widget(errs, valid_widget(), *W::find("posts_summary"), 0);
    EXPECT_FALSE(errs.any());
}

TEST(WidgetCatalog, RejectsNonIntegerGeometry) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["grid_x"] = 1.5;
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].field, "widgets[0].grid_x");
    EXPECT_EQ(errs.items()[0].code, "not_integer");
}

TEST(WidgetCatalog, RejectsNegativeGeometry) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["grid_y"] = -1;
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "below_min");
}

TEST(WidgetCatalog, RejectsMissingGeometry) {
    Api::Validation::Errors errs;
    json w = json{{"widget_type", "posts_summary"}, {"grid_x", 0}};
    W::validate_widget(errs, w, *W::find("posts_summary"), 2);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].field, "widgets[2].grid_y");
    EXPECT_EQ(errs.items()[0].code, "missing");
}

TEST(WidgetCatalog, RejectsWidthBelowSpecMinimum) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["grid_w"] = 1;  // posts_summary declares min_w = 3
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].field, "widgets[0].grid_w");
}

TEST(WidgetCatalog, RejectsWidgetRunningPastTheGrid) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["grid_x"] = 10;
    w["grid_w"] = 4;  // 10 + 4 > 12
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "out_of_grid");
}

TEST(WidgetCatalog, RejectsUnknownOptionKey) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["options"] = json{{"colour", 3}};
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].field, "widgets[0].options.colour");
    EXPECT_EQ(errs.items()[0].code, "unknown_option");
}

TEST(WidgetCatalog, RejectsOptionOutOfRange) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["options"] = json{{"limit", 999}};  // posts_summary caps limit at 10
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "above_max");
}

TEST(WidgetCatalog, RejectsNonObjectOptions) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["options"] = "limit=5";
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "not_object");
}

}  // namespace
```

- [ ] **Step 2: Run test to verify it fails**

```sh
make test-unit 2>&1 | tail -30
```

Expected: сборка падает на `domain/WidgetCatalog.hpp: No such file or directory`.

Если цели `test-unit` нет, посмотреть `make help` и взять соответствующую (юнит-бакет собирается из `tests/unit/*.cpp` глобом с `CONFIGURE_DEPENDS`, регистрировать файл не нужно).

- [ ] **Step 3: Write minimal implementation**

Создать `src/domain/WidgetCatalog.hpp`:

```cpp
/**
 * @file WidgetCatalog.hpp
 * @brief The dashboard widget catalog: the single list of widget types the
 *        backend accepts and the frontend renders. GET /api/v1/dashboard/catalog
 *        publishes it; PUT /api/v1/dashboard/layout validates against it. Keeping
 *        both on one constexpr array is what stops a second list from drifting.
 *
 * Pure logic: no HTTP, no SQL. The permission FILTER is injected by the caller
 * (the controller passes Security::Auth::require_permission), so this header
 * stays testable without booting Core.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "api/Validation.hpp"
#include "domain/Role.hpp"

namespace Domain::Widgets {

using json = nlohmann::json;

/// The frontend renders a 12-column grid; these bounds are what the API enforces.
inline constexpr int kGridColumns = 12;
inline constexpr int kMaxGridY = 200;
inline constexpr int kMaxGridH = 24;
inline constexpr std::size_t kMaxWidgets = 40;

/// One integer option a widget accepts. Widgets have no string options yet —
/// adding one means a new spec kind here, never a free-form passthrough.
struct OptionSpec {
    std::string_view key;
    long long min_value;
    long long max_value;
    long long default_value;
};

struct WidgetSpec {
    std::string_view type;
    std::string_view title;
    std::string_view description;
    /// Permission bit the caller must hold. kAdminister for admin-only widgets.
    std::uint32_t required_permission;
    int default_w;
    int default_h;
    int min_w;
    int min_h;
    const OptionSpec* options;
    std::size_t option_count;
};

inline constexpr std::array<OptionSpec, 1> kPostsOptions{{{"limit", 1, 10, 5}}};
inline constexpr std::array<OptionSpec, 1> kJobsOptions{{{"window_days", 1, 30, 7}}};
inline constexpr std::array<OptionSpec, 1> kAuditOptions{{{"limit", 1, 20, 10}}};
inline constexpr std::array<OptionSpec, 1> kUsersOptions{{{"limit", 1, 10, 5}}};
inline constexpr std::array<OptionSpec, 1> kNoOptions{{{"", 0, 0, 0}}};

/// Order here is the order a fresh dashboard offers them in.
inline constexpr std::array<WidgetSpec, 5> kCatalog{{
    {"posts_summary",
     "Posts",
     "Drafts, published and the latest entries",
     Permission::kAdminister,
     4,
     3,
     3,
     2,
     kPostsOptions.data(),
     kPostsOptions.size()},
    {"jobs_queue",
     "Jobs",
     "Queue by status and DLQ size",
     Permission::kAdminister,
     4,
     3,
     3,
     2,
     kJobsOptions.data(),
     kJobsOptions.size()},
    {"audit_recent",
     "Audit",
     "Latest admin actions",
     Permission::kAuditRead,
     6,
     4,
     4,
     3,
     kAuditOptions.data(),
     kAuditOptions.size()},
    {"users_recent",
     "Users",
     "User count and latest registrations",
     Permission::kAdminister,
     4,
     3,
     3,
     2,
     kUsersOptions.data(),
     kUsersOptions.size()},
    // service_health takes no options: kNoOptions carries a single empty-key
    // entry (a zero-length std::array is legal but data() may be nullptr), and
    // option_count = 0 keeps it invisible to both the JSON and the validator.
    {"service_health", "Service", "Readiness and version", Permission::kAdminister, 4, 2, 2, 2, kNoOptions.data(), 0},
}};

inline const WidgetSpec* find(std::string_view type) {
    for (const auto& spec : kCatalog)
        if (spec.type == type)
            return &spec;
    return nullptr;
}

/**
 * @brief Serialize the catalog, keeping only specs the caller may place.
 * @param permitted Predicate over a permission bit: true when the caller holds
 *        it. The controller passes a lambda over
 *        Security::Auth::require_permission so the auth-disabled escape hatch
 *        behaves exactly as it does for every other guard.
 */
template <typename PermitFn>
inline json catalog_json(PermitFn&& permitted) {
    json out = json::array();
    for (const auto& spec : kCatalog) {
        if (!permitted(spec.required_permission))
            continue;
        json options = json::object();
        for (std::size_t i = 0; i < spec.option_count; ++i) {
            const auto& opt = spec.options[i];
            options[std::string(opt.key)] = {{"type", "int"},
                                             {"min", opt.min_value},
                                             {"max", opt.max_value},
                                             {"default", opt.default_value}};
        }
        out.push_back({{"type", std::string(spec.type)},
                       {"title", std::string(spec.title)},
                       {"description", std::string(spec.description)},
                       {"default_w", spec.default_w},
                       {"default_h", spec.default_h},
                       {"min_w", spec.min_w},
                       {"min_h", spec.min_h},
                       {"options", options}});
    }
    return out;
}

namespace detail {

/// Read an integer field within [min_v, max_v], reporting under `reported`
/// (which carries the widget index) instead of the bare key. Hand-rolled rather
/// than Validation::int_range because that one reports the bare field name, and
/// a 400 on a 12-widget layout must say WHICH widget is wrong.
inline bool int_field(Api::Validation::Errors& errs,
                      const json& obj,
                      const std::string& key,
                      const std::string& reported,
                      long long min_v,
                      long long max_v,
                      long long& out) {
    if (!obj.contains(key) || obj[key].is_null()) {
        errs.add(reported, "missing", "required");
        return false;
    }
    // is_number_integer() is false for 1.5 — without this check the get<>
    // below throws nlohmann's type_error.302, which escapes as a bare 500.
    if (!obj[key].is_number_integer()) {
        errs.add(reported, "not_integer", "must be an integer");
        return false;
    }
    out = obj[key].get<long long>();
    if (out < min_v) {
        errs.add(reported, "below_min", "min " + std::to_string(min_v));
        return false;
    }
    if (out > max_v) {
        errs.add(reported, "above_max", "max " + std::to_string(max_v));
        return false;
    }
    return true;
}

}  // namespace detail

/**
 * @brief Validate one widget's geometry and options against its spec.
 * @param index Position in the request's `widgets` array; every error field is
 *        prefixed `widgets[<index>].` so the client can point at the entry.
 */
inline void validate_widget(Api::Validation::Errors& errs, const json& widget, const WidgetSpec& spec, std::size_t index) {
    const std::string prefix = "widgets[" + std::to_string(index) + "].";
    long long x = 0;
    long long width = 0;
    long long scratch = 0;

    const bool have_x = detail::int_field(errs, widget, "grid_x", prefix + "grid_x", 0, kGridColumns - 1, x);
    detail::int_field(errs, widget, "grid_y", prefix + "grid_y", 0, kMaxGridY, scratch);
    const bool have_w = detail::int_field(errs, widget, "grid_w", prefix + "grid_w", spec.min_w, kGridColumns, width);
    detail::int_field(errs, widget, "grid_h", prefix + "grid_h", spec.min_h, kMaxGridH, scratch);

    if (have_x && have_w && x + width > kGridColumns) {
        errs.add(prefix + "grid_w",
                 "out_of_grid",
                 "grid_x + grid_w must not exceed " + std::to_string(kGridColumns));
    }

    // options is optional; absent means "spec defaults".
    if (!widget.contains("options") || widget["options"].is_null())
        return;
    if (!widget["options"].is_object()) {
        errs.add(prefix + "options", "not_object", "must be an object");
        return;
    }
    for (auto it = widget["options"].begin(); it != widget["options"].end(); ++it) {
        const OptionSpec* opt = nullptr;
        for (std::size_t i = 0; i < spec.option_count; ++i) {
            if (spec.options[i].key == it.key()) {
                opt = &spec.options[i];
                break;
            }
        }
        if (!opt) {
            // Silently dropping an unknown key would let a stale client think a
            // setting took effect. Fail loudly instead.
            errs.add(prefix + "options." + it.key(), "unknown_option", "not accepted by " + std::string(spec.type));
            continue;
        }
        detail::int_field(errs,
                          widget["options"],
                          std::string(opt->key),
                          prefix + "options." + it.key(),
                          opt->min_value,
                          opt->max_value,
                          scratch);
    }
}

}  // namespace Domain::Widgets
```

- [ ] **Step 4: Run test to verify it passes**

```sh
make test-unit 2>&1 | tail -30
```

Expected: все одиннадцать кейсов `WidgetCatalog.*` проходят.

- [ ] **Step 5: Commit**

```bash
git add src/domain/WidgetCatalog.hpp tests/unit/test_widget_catalog.cpp
git commit -m "feat(dashboard): widget catalog with geometry and option validation"
```

---

### Task 2: Таблица раскладки, DTO и репозиторий

**Files:**
- Create: `migrations/010_dashboard_widgets.sql`
- Create: `src/domain/DashboardWidget.hpp`
- Create: `src/repositories/DashboardWidgetRepository.hpp`
- Test: `tests/integration/test_dashboard_layout.cpp`

**Interfaces:**
- Consumes: `Repositories::CrudBase` (`src/repositories/CrudBase.hpp`), `Database::get()` (`src/database/Database.hpp`), `Utils::Time::pg_to_iso8601` (`src/utils/Time.hpp`), `Domain::Widgets::kMaxWidgets` из задачи 1.
- Produces:
  - `struct Domain::DashboardWidget { std::string id, owner_id, widget_type; int grid_x, grid_y, grid_w, grid_h; nlohmann::json options; std::string created_at, updated_at; }` с `from_row` и ADL-`to_json`
  - `struct Repositories::DashboardWidgetInput { std::string widget_type; int grid_x, grid_y, grid_w, grid_h; std::string options_json; }`
  - `class Repositories::DashboardWidgetRepository` с `list_owned(owner, limit, offset)`, `count_owned(owner)`, `find_owned(id, owner)` от `CrudBase` и `std::vector<Domain::DashboardWidget> replace_all(const std::string& owner_id, const std::vector<DashboardWidgetInput>&)`

- [ ] **Step 1: Write the failing test**

Создать `tests/integration/test_dashboard_layout.cpp` (в этой задаче только блок репозитория, ручки добавляются в задаче 3):

```cpp
/**
 * @file test_dashboard_layout.cpp
 * @brief Dashboard layout: repository replace semantics, owner isolation and
 *        the HTTP contract of /api/v1/dashboard/{catalog,layout}.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "domain/Role.hpp"
#include "repositories/DashboardWidgetRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

class DashboardLayoutTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // CASCADE also clears dashboard_widgets (FK ON DELETE CASCADE).
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    /// Seed a user and return the principal a handler would see. `permissions`
    /// lands in the JWT claims, which is what current_user_can reads.
    Security::Auth::AuthPrincipal seed_user(const std::string& email, std::uint32_t permissions) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        EXPECT_TRUE(role.has_value());
        auto user = users.create(email, std::string("$argon2id$x"), std::nullopt, std::nullopt, role->id, true);
        Security::Auth::AuthPrincipal p;
        p.subject = user.id;
        p.raw_claims = json{{"sub", user.id}, {"permissions", permissions}};
        return p;
    }

    static Repositories::DashboardWidgetInput widget(const std::string& type,
                                                     int x,
                                                     int y,
                                                     int w,
                                                     int h,
                                                     const std::string& options = "{}") {
        Repositories::DashboardWidgetInput in;
        in.widget_type = type;
        in.grid_x = x;
        in.grid_y = y;
        in.grid_w = w;
        in.grid_h = h;
        in.options_json = options;
        return in;
    }
};

TEST_F(DashboardLayoutTest, RepositoryStoresAndReadsBackInGridOrder) {
    auto owner = seed_user("owner@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;

    auto stored = repo.replace_all(owner.subject,
                                   {widget("jobs_queue", 0, 1, 4, 3),
                                    widget("posts_summary", 0, 0, 4, 3, R"({"limit":7})")});
    ASSERT_EQ(stored.size(), 2u);

    auto read = repo.list_owned(owner.subject, 40, 0);
    ASSERT_EQ(read.size(), 2u);
    // kOrderBy is "grid_y, grid_x", so row 0 comes first regardless of insert order.
    EXPECT_EQ(read[0].widget_type, "posts_summary");
    EXPECT_EQ(read[1].widget_type, "jobs_queue");
    // options is jsonb in the table and a parsed object in the DTO.
    EXPECT_TRUE(read[0].options.is_object());
    EXPECT_EQ(read[0].options["limit"], 7);
    EXPECT_EQ(read[1].options, json::object());
}

TEST_F(DashboardLayoutTest, ReplaceAllOverwritesTheWholeSet) {
    auto owner = seed_user("replace@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;

    repo.replace_all(owner.subject, {widget("posts_summary", 0, 0, 4, 3), widget("jobs_queue", 4, 0, 4, 3)});
    auto second = repo.replace_all(owner.subject, {widget("service_health", 0, 0, 4, 2)});

    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(repo.count_owned(owner.subject), 1);
    EXPECT_EQ(repo.list_owned(owner.subject, 40, 0)[0].widget_type, "service_health");
}

TEST_F(DashboardLayoutTest, ReplaceAllWithEmptySetClearsTheLayout) {
    auto owner = seed_user("clear@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;

    repo.replace_all(owner.subject, {widget("posts_summary", 0, 0, 4, 3)});
    auto cleared = repo.replace_all(owner.subject, {});

    EXPECT_TRUE(cleared.empty());
    EXPECT_EQ(repo.count_owned(owner.subject), 0);
}

TEST_F(DashboardLayoutTest, OneOwnerNeverSeesAnothersLayout) {
    auto alice = seed_user("alice@example.com", Domain::Permission::kAdminister);
    auto bob = seed_user("bob@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;

    auto alice_rows = repo.replace_all(alice.subject, {widget("posts_summary", 0, 0, 4, 3)});
    repo.replace_all(bob.subject, {widget("jobs_queue", 0, 0, 4, 3), widget("service_health", 4, 0, 4, 2)});

    EXPECT_EQ(repo.count_owned(alice.subject), 1);
    EXPECT_EQ(repo.count_owned(bob.subject), 2);
    // find_owned is the only lookup a handler may use: Bob cannot fetch Alice's row.
    EXPECT_FALSE(repo.find_owned(alice_rows[0].id, bob.subject).has_value());
    EXPECT_TRUE(repo.find_owned(alice_rows[0].id, alice.subject).has_value());
}

TEST_F(DashboardLayoutTest, DeletingTheUserCascadesTheLayout) {
    auto owner = seed_user("cascade@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;
    repo.replace_all(owner.subject, {widget("posts_summary", 0, 0, 4, 3)});

    Repositories::UserRepository users;
    users.remove(owner.subject);

    EXPECT_EQ(repo.count_owned(owner.subject), 0);
}

}  // namespace
```

Если метод удаления в `UserRepository` называется не `remove`, взять фактическое имя:

```sh
grep -n 'bool remove\|void remove\|delete_by_id' src/repositories/UserRepository.hpp
```

- [ ] **Step 2: Run test to verify it fails**

```sh
make test-integration 2>&1 | tail -30
```

Expected: сборка падает на `repositories/DashboardWidgetRepository.hpp: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Создать `migrations/010_dashboard_widgets.sql`:

```sql
-- Migration 010: dashboard_widgets
--
-- The MigrationRunner already wraps this file in ONE transaction (under an
-- advisory lock) together with the schema_migrations bookkeeping. Do NOT add
-- BEGIN/COMMIT — an embedded COMMIT ends that transaction early and breaks
-- atomicity. Prefer idempotent DDL (IF NOT EXISTS).

-- One row per widget instance on a user's dashboard. Type, position and size
-- are columns (not one JSON blob) so bounds validation and ordering stay in
-- SQL; `options` holds only the widget's own parameters (period, limit).
CREATE TABLE IF NOT EXISTS dashboard_widgets (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id    UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    widget_type VARCHAR(64) NOT NULL,       -- validated against Domain::Widgets::kCatalog
    grid_x      SMALLINT    NOT NULL,       -- 0..11 on the 12-column grid
    grid_y      SMALLINT    NOT NULL,
    grid_w      SMALLINT    NOT NULL,
    grid_h      SMALLINT    NOT NULL,
    options     JSONB       NOT NULL DEFAULT '{}'::jsonb,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- The only read pattern: one owner's layout in grid order.
CREATE INDEX IF NOT EXISTS idx_dashboard_widgets_owner
    ON dashboard_widgets (owner_id, grid_y, grid_x);

-- Bump updated_at on every UPDATE via the shared function from
-- migrations/000_updated_at_trigger.sql.
DROP TRIGGER IF EXISTS dashboard_widgets_touch_updated_at ON dashboard_widgets;
CREATE TRIGGER dashboard_widgets_touch_updated_at
    BEFORE UPDATE ON dashboard_widgets
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
```

Создать `src/domain/DashboardWidget.hpp`:

```cpp
/**
 * @file DashboardWidget.hpp
 * @brief One widget instance on a user's dashboard (dashboard_widgets table,
 *        migration 010). Type, position and size are columns; `options` carries
 *        the widget's own parameters and is validated against the catalog
 *        (domain/WidgetCatalog.hpp) before it ever reaches this DTO.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "utils/Time.hpp"

namespace Domain {

struct DashboardWidget {
    std::string id;
    std::string owner_id;
    std::string widget_type;
    int grid_x{0};
    int grid_y{0};
    int grid_w{0};
    int grid_h{0};
    nlohmann::json options = nlohmann::json::object();
    // Timestamps are ISO 8601 — normalized in from_row(), as Post and ApiKey do.
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static DashboardWidget from_row(const Row& row) {
        DashboardWidget w;
        w.id = row["id"].template as<std::string>();
        w.owner_id = row["owner_id"].template as<std::string>();
        w.widget_type = row["widget_type"].template as<std::string>();
        w.grid_x = row["grid_x"].template as<int>();
        w.grid_y = row["grid_y"].template as<int>();
        w.grid_w = row["grid_w"].template as<int>();
        w.grid_h = row["grid_h"].template as<int>();
        // jsonb comes back as text (kColumns casts it explicitly). Parse at this
        // single DB→domain boundary so no handler ever sees a string where the
        // contract promises an object.
        w.options = nlohmann::json::parse(row["options"].template as<std::string>());
        w.created_at = Utils::Time::pg_to_iso8601(row["created_at"].template as<std::string>());
        w.updated_at = Utils::Time::pg_to_iso8601(row["updated_at"].template as<std::string>());
        return w;
    }
};

inline void to_json(nlohmann::json& j, const DashboardWidget& w) {
    // owner_id is intentionally absent: the caller IS the owner on every route
    // that serves this DTO, so echoing a user id would add nothing but exposure.
    j = nlohmann::json{{"id", w.id},
                       {"widget_type", w.widget_type},
                       {"grid_x", w.grid_x},
                       {"grid_y", w.grid_y},
                       {"grid_w", w.grid_w},
                       {"grid_h", w.grid_h},
                       {"options", w.options},
                       {"created_at", w.created_at},
                       {"updated_at", w.updated_at}};
}

}  // namespace Domain
```

Создать `src/repositories/DashboardWidgetRepository.hpp`:

```cpp
/**
 * @file DashboardWidgetRepository.hpp
 * @brief Owner-scoped access to dashboard_widgets (migration 010). CrudBase
 *        supplies the reads; the one hand-written write replaces a whole layout,
 *        because the grid always hands over a complete arrangement.
 */

#pragma once

#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/DashboardWidget.hpp"
#include "repositories/CrudBase.hpp"

namespace Repositories {

/// Validated input row the controller hands to replace_all. `options_json` is
/// already-serialized JSON (the controller dumps the validated object), so the
/// repository never needs to know the option rules.
struct DashboardWidgetInput {
    std::string widget_type;
    int grid_x{0};
    int grid_y{0};
    int grid_w{0};
    int grid_h{0};
    std::string options_json{"{}"};
};

class DashboardWidgetRepository : public CrudBase<DashboardWidgetRepository, Domain::DashboardWidget, std::string> {
public:
    static constexpr const char* kTable = "dashboard_widgets";
    // options::text so libpqxx hands the DTO a string it can parse — reading
    // jsonb as std::string directly is driver-dependent.
    static constexpr const char* kColumns =
        "id, owner_id, widget_type, grid_x, grid_y, grid_w, grid_h, options::text AS options, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "grid_y, grid_x";
    // Unlocks find_owned / list_owned / count_owned. Plain find/list on this
    // table would be an IDOR — never call them.
    static constexpr const char* kOwnerColumn = "owner_id";

    /**
     * @brief Replace the owner's entire layout in one transaction.
     * @details Drag-and-drop produces a whole new arrangement, so there is no
     *          per-widget patch: delete-then-insert inside a single write keeps
     *          the layout from ever being half-applied. Returns the stored rows
     *          (fresh ids) in insertion order.
     */
    std::vector<Domain::DashboardWidget> replace_all(const std::string& owner_id,
                                                     const std::vector<DashboardWidgetInput>& widgets) {
        return Database::get().execute_write([&](auto& txn) {
            txn.exec_params("DELETE FROM dashboard_widgets WHERE owner_id = $1", owner_id);
            std::vector<Domain::DashboardWidget> out;
            out.reserve(widgets.size());
            for (const auto& w : widgets) {
                auto r = txn.exec_params(
                    std::string("INSERT INTO dashboard_widgets "
                                "(owner_id, widget_type, grid_x, grid_y, grid_w, grid_h, options) "
                                "VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb) RETURNING ") +
                        kColumns,
                    owner_id,
                    w.widget_type,
                    w.grid_x,
                    w.grid_y,
                    w.grid_w,
                    w.grid_h,
                    w.options_json);
                out.push_back(Domain::DashboardWidget::from_row(r[0]));
            }
            return out;
        });
    }
};

}  // namespace Repositories
```

- [ ] **Step 4: Run test to verify it passes**

```sh
make up
make test-integration 2>&1 | tail -40
```

Expected: пять кейсов `DashboardLayoutTest.*` проходят. Если суита пропущена, значит Postgres недоступен: поднять `make up` и повторить.

- [ ] **Step 5: Commit**

```bash
git add migrations/010_dashboard_widgets.sql src/domain/DashboardWidget.hpp \
        src/repositories/DashboardWidgetRepository.hpp tests/integration/test_dashboard_layout.cpp
git commit -m "feat(dashboard): dashboard_widgets table, DTO and owner-scoped repository"
```

---

### Task 3: Ручки раскладки, реестр роутов и OpenAPI

**Files:**
- Create: `src/api/DashboardController.hpp`
- Modify: `src/api/Endpoints.hpp` (три строки в `get_endpoints()`)
- Modify: `src/api/Api.hpp` (один `#include`)
- Modify: `docs/openapi.yaml` (два блока путей и три схемы)
- Test: `tests/integration/test_dashboard_layout.cpp` (дописать блок HTTP)

**Interfaces:**
- Consumes: `Domain::Widgets::find`, `catalog_json`, `validate_widget`, `kMaxWidgets` (задача 1); `Repositories::DashboardWidgetRepository`, `DashboardWidgetInput` (задача 2); `API_REQUIRE_OWNER` (`src/api/Guards.hpp`); `Api::with_repo_errors` (`src/api/HandlerSupport.hpp`); `Response::ok` и `Response::list` (`src/utils/ErrorResponse.hpp`); `Security::Auth::require_permission` (`src/security/Auth.hpp`).
- Produces: HTTP-контракт, на который опирается фронт задач 11 и 12:
  - `GET /api/v1/dashboard/catalog` → `{"data":[{type,title,description,default_w,default_h,min_w,min_h,options}]}`
  - `GET /api/v1/dashboard/layout` → `{"data":[{id,widget_type,grid_x,grid_y,grid_w,grid_h,options,created_at,updated_at}]}`
  - `PUT /api/v1/dashboard/layout` с телом `{"widgets":[{widget_type,grid_x,grid_y,grid_w,grid_h,options?}]}` → 200 с тем же конвертом, что у `GET`

- [ ] **Step 1: Write the failing test**

Дописать в `tests/integration/test_dashboard_layout.cpp`. Добавить включения в начало файла:

```cpp
#include <functional>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "api/DashboardController.hpp"
```

И внутрь анонимного namespace, после существующих кейсов, добавить фикстуру с хелперами и кейсы:

```cpp
using namespace drogon;

class DashboardApiTest : public DashboardLayoutTest {
protected:
    Api::DashboardController controller;

    json get_catalog(const Security::Auth::AuthPrincipal& p, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.catalog(TestHelpers::authed(p), [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    json get_layout(const Security::Auth::AuthPrincipal& p, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.layout(TestHelpers::authed(p), [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    json put_layout(const Security::Auth::AuthPrincipal& p, const json& body, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.saveLayout(TestHelpers::authed_json(p, body, drogon::Put),
                              [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    static json widget_json(const char* type, int x, int y, int w, int h) {
        return json{{"widget_type", type}, {"grid_x", x}, {"grid_y", y}, {"grid_w", w}, {"grid_h", h}};
    }
};

TEST_F(DashboardApiTest, CatalogIsFilteredByPermission) {
    auto admin = seed_user("admin-cat@example.com", Domain::Permission::kAdminister);
    auto auditor = seed_user("auditor-cat@example.com", Domain::Permission::kAuditRead);

    EXPECT_EQ(get_catalog(admin)["data"].size(), 5u);

    auto limited = get_catalog(auditor)["data"];
    ASSERT_EQ(limited.size(), 1u);
    EXPECT_EQ(limited[0]["type"], "audit_recent");
}

TEST_F(DashboardApiTest, LayoutStartsEmptyAndRoundTrips) {
    auto owner = seed_user("roundtrip@example.com", Domain::Permission::kAdminister);
    EXPECT_TRUE(get_layout(owner)["data"].empty());

    int status = 0;
    auto saved = put_layout(owner,
                            json{{"widgets",
                                  json::array({widget_json("posts_summary", 0, 0, 4, 3),
                                               widget_json("jobs_queue", 4, 0, 4, 3)})}},
                            &status);
    ASSERT_EQ(status, k200OK);
    ASSERT_EQ(saved["data"].size(), 2u);
    EXPECT_TRUE(saved["data"][0].contains("id"));
    // owner_id never leaves the DTO.
    EXPECT_FALSE(saved["data"][0].contains("owner_id"));

    auto read = get_layout(owner)["data"];
    ASSERT_EQ(read.size(), 2u);
    EXPECT_EQ(read[0]["widget_type"], "posts_summary");
    EXPECT_EQ(read[0]["options"], json::object());
}

TEST_F(DashboardApiTest, EmptyWidgetArrayClearsTheLayout) {
    auto owner = seed_user("empty@example.com", Domain::Permission::kAdminister);
    put_layout(owner, json{{"widgets", json::array({widget_json("posts_summary", 0, 0, 4, 3)})}});

    int status = 0;
    auto cleared = put_layout(owner, json{{"widgets", json::array()}}, &status);
    EXPECT_EQ(status, k200OK);
    EXPECT_TRUE(cleared["data"].empty());
    EXPECT_TRUE(get_layout(owner)["data"].empty());
}

TEST_F(DashboardApiTest, TwoWidgetsOfTheSameTypeAreAllowed) {
    auto owner = seed_user("twins@example.com", Domain::Permission::kAdminister);
    auto a = widget_json("posts_summary", 0, 0, 4, 3);
    a["options"] = json{{"limit", 3}};
    auto b = widget_json("posts_summary", 4, 0, 4, 3);
    b["options"] = json{{"limit", 10}};

    int status = 0;
    auto saved = put_layout(owner, json{{"widgets", json::array({a, b})}}, &status);
    ASSERT_EQ(status, k200OK);
    ASSERT_EQ(saved["data"].size(), 2u);
    EXPECT_EQ(saved["data"][0]["options"]["limit"], 3);
    EXPECT_EQ(saved["data"][1]["options"]["limit"], 10);
}

TEST_F(DashboardApiTest, RejectsUnknownWidgetType) {
    auto owner = seed_user("unknown@example.com", Domain::Permission::kAdminister);
    int status = 0;
    auto body = put_layout(owner, json{{"widgets", json::array({widget_json("nope", 0, 0, 4, 3)})}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["error"], "validation_failed");
    EXPECT_EQ(body["errors"][0]["field"], "widgets[0].widget_type");
    EXPECT_EQ(body["errors"][0]["code"], "unknown_widget");
}

TEST_F(DashboardApiTest, RejectsFractionalGeometryWithA400NotA500) {
    auto owner = seed_user("fraction@example.com", Domain::Permission::kAdminister);
    auto w = widget_json("posts_summary", 0, 0, 4, 3);
    w["grid_x"] = 1.5;
    int status = 0;
    auto body = put_layout(owner, json{{"widgets", json::array({w})}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["errors"][0]["field"], "widgets[0].grid_x");
    EXPECT_EQ(body["errors"][0]["code"], "not_integer");
}

TEST_F(DashboardApiTest, RejectsOptionOutOfRange) {
    auto owner = seed_user("range@example.com", Domain::Permission::kAdminister);
    auto w = widget_json("posts_summary", 0, 0, 4, 3);
    w["options"] = json{{"limit", 999}};
    int status = 0;
    auto body = put_layout(owner, json{{"widgets", json::array({w})}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["errors"][0]["code"], "above_max");
}

TEST_F(DashboardApiTest, RejectsMoreThanFortyWidgets) {
    auto owner = seed_user("flood@example.com", Domain::Permission::kAdminister);
    json widgets = json::array();
    for (int i = 0; i < 41; ++i)
        widgets.push_back(widget_json("service_health", 0, i, 4, 2));
    int status = 0;
    auto body = put_layout(owner, json{{"widgets", widgets}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["errors"][0]["code"], "too_many");
}

TEST_F(DashboardApiTest, RejectsMissingWidgetsArray) {
    auto owner = seed_user("nobody@example.com", Domain::Permission::kAdminister);
    int status = 0;
    auto body = put_layout(owner, json{{"layout", json::array()}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["errors"][0]["field"], "widgets");
}

TEST_F(DashboardApiTest, PlacingAWidgetWithoutItsPermissionIs403) {
    auto auditor = seed_user("auditor-put@example.com", Domain::Permission::kAuditRead);
    int status = 0;
    // posts_summary needs kAdminister; the auditor holds only kAuditRead.
    put_layout(auditor, json{{"widgets", json::array({widget_json("posts_summary", 0, 0, 4, 3)})}}, &status);
    EXPECT_EQ(status, k403Forbidden);

    // Its own widget still goes through.
    int ok_status = 0;
    put_layout(auditor, json{{"widgets", json::array({widget_json("audit_recent", 0, 0, 6, 4)})}}, &ok_status);
    EXPECT_EQ(ok_status, k200OK);
}

TEST_F(DashboardApiTest, LayoutsAreIsolatedPerCaller) {
    auto alice = seed_user("alice-api@example.com", Domain::Permission::kAdminister);
    auto bob = seed_user("bob-api@example.com", Domain::Permission::kAdminister);

    put_layout(alice, json{{"widgets", json::array({widget_json("posts_summary", 0, 0, 4, 3)})}});
    EXPECT_TRUE(get_layout(bob)["data"].empty());

    put_layout(bob, json{{"widgets", json::array({widget_json("jobs_queue", 0, 0, 4, 3)})}});
    EXPECT_EQ(get_layout(alice)["data"].size(), 1u);
    EXPECT_EQ(get_layout(alice)["data"][0]["widget_type"], "posts_summary");
}
```

Если `TestHelpers::authed` не принимает метод запроса, посмотреть его сигнатуру и при необходимости собрать запрос через `TestHelpers::with_principal(TestHelpers::make_request(drogon::Get), p)`:

```sh
grep -n 'inline drogon::HttpRequestPtr authed\|with_principal\|make_request' tests/test_helpers.hpp
```

- [ ] **Step 2: Run test to verify it fails**

```sh
make test-integration 2>&1 | tail -30
```

Expected: сборка падает на `api/DashboardController.hpp: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Создать `src/api/DashboardController.hpp`:

```cpp
/**
 * @file DashboardController.hpp
 * @brief The caller's own dashboard: the widget catalog and the saved layout.
 *        Owner-scoped (API_REQUIRE_OWNER) so one user never reads or writes
 *        another's arrangement. PUT replaces the whole layout: dragging a card
 *        produces a new arrangement, not a patch, and a full replace is
 *        idempotent for free.
 *
 * Deliberately NOT validated: overlaps between widgets. react-grid-layout never
 * produces them, and rejecting them here would add a 400 with no reader behind it.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <drogon/HttpController.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/Validation.hpp"
#include "domain/WidgetCatalog.hpp"
#include "repositories/DashboardWidgetRepository.hpp"
#include "security/Auth.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class DashboardController : public HttpController<DashboardController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DashboardController::catalog, "/api/v1/dashboard/catalog", Get);
    ADD_METHOD_TO(DashboardController::layout, "/api/v1/dashboard/layout", Get);
    ADD_METHOD_TO(DashboardController::saveLayout, "/api/v1/dashboard/layout", Put);
    METHOD_LIST_END

    /// The widget types this caller may place. Filtering here (rather than only
    /// on PUT) is what keeps a widget the caller can't use out of the picker.
    void catalog(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_OWNER(req, callback, owner);
        (void)owner;  // identity is required; the catalog itself is per-permission
        callback(Response::list(Domain::Widgets::catalog_json(
            // require_permission returns nullptr when the caller holds the bit
            // AND when auth is disabled — reusing it keeps this gate identical
            // to every other one instead of re-deriving the dev-mode escape.
            [&](std::uint32_t perm) { return !Security::Auth::require_permission(req, perm); })));
    }

    void layout(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_OWNER(req, callback, owner);
        Repositories::DashboardWidgetRepository repo;
        with_repo_errors(callback, "dashboard layout", [&] {
            auto widgets = repo.list_owned(owner, static_cast<int>(Domain::Widgets::kMaxWidgets), 0);
            json data = json::array();
            for (const auto& w : widgets)
                data.push_back(w);
            callback(Response::list(data));
        });
    }

    void saveLayout(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_OWNER(req, callback, owner);

        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        if (!body.contains("widgets") || !body["widgets"].is_array()) {
            errs.add("widgets", "not_array", "must be an array");
            callback(Validation::response_400(errs));
            return;
        }
        const auto& items = body["widgets"];
        if (items.size() > Domain::Widgets::kMaxWidgets) {
            errs.add("widgets", "too_many", "max " + std::to_string(Domain::Widgets::kMaxWidgets));
            callback(Validation::response_400(errs));
            return;
        }

        std::vector<Repositories::DashboardWidgetInput> inputs;
        inputs.reserve(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& w = items[i];
            const std::string field = "widgets[" + std::to_string(i) + "]";
            if (!w.is_object()) {
                errs.add(field, "not_object", "must be an object");
                continue;
            }
            if (!w.contains("widget_type") || !w["widget_type"].is_string()) {
                errs.add(field + ".widget_type", "invalid", "must be a string");
                continue;
            }
            const auto type = w["widget_type"].get<std::string>();
            const auto* spec = Domain::Widgets::find(type);
            if (!spec) {
                errs.add(field + ".widget_type", "unknown_widget", "not in the catalog");
                continue;
            }
            // A widget the caller may not read is an access question, not a
            // shape question — 403, and the whole request stops here.
            if (auto forbidden = Security::Auth::require_permission(req, spec->required_permission)) {
                callback(forbidden);
                return;
            }

            const std::size_t before = errs.items().size();
            Domain::Widgets::validate_widget(errs, w, *spec, i);
            if (errs.items().size() != before)
                continue;  // keep collecting errors from later widgets, skip this one

            Repositories::DashboardWidgetInput in;
            in.widget_type = type;
            in.grid_x = w["grid_x"].get<int>();
            in.grid_y = w["grid_y"].get<int>();
            in.grid_w = w["grid_w"].get<int>();
            in.grid_h = w["grid_h"].get<int>();
            in.options_json = (w.contains("options") && w["options"].is_object()) ? w["options"].dump() : "{}";
            inputs.push_back(std::move(in));
        }

        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        Repositories::DashboardWidgetRepository repo;
        with_repo_errors(callback, "dashboard saveLayout", [&] {
            auto stored = repo.replace_all(owner, inputs);
            json data = json::array();
            for (const auto& w : stored)
                data.push_back(w);
            callback(Response::list(data));
        });
    }
};

}  // namespace Api
```

Дописать в `src/api/Endpoints.hpp`, в `get_endpoints()`, сразу после трех строк `/api/v1/account/api-keys`:

```cpp
        {"GET", "/api/v1/dashboard/catalog", "Widget types you may place on your dashboard"},
        {"GET", "/api/v1/dashboard/layout", "Your dashboard layout"},
        {"PUT", "/api/v1/dashboard/layout", "Replace your dashboard layout"},
```

Дописать в `src/api/Api.hpp` к остальным включениям контроллеров:

```cpp
#include "api/DashboardController.hpp"
```

Дописать в `docs/openapi.yaml`. В `components.schemas` (рядом с остальными схемами):

```yaml
    DashboardWidget:
      type: object
      required: [id, widget_type, grid_x, grid_y, grid_w, grid_h, options, created_at, updated_at]
      properties:
        id: { type: string, format: uuid }
        widget_type: { type: string, description: "A type from /api/v1/dashboard/catalog" }
        grid_x: { type: integer, minimum: 0, maximum: 11 }
        grid_y: { type: integer, minimum: 0, maximum: 200 }
        grid_w: { type: integer, minimum: 1, maximum: 12 }
        grid_h: { type: integer, minimum: 1, maximum: 24 }
        options: { type: object, additionalProperties: { type: integer } }
        created_at: { type: string, format: date-time }
        updated_at: { type: string, format: date-time }
    DashboardLayoutResponse:
      type: object
      required: [data]
      properties:
        data: { type: array, items: { $ref: '#/components/schemas/DashboardWidget' } }
    DashboardCatalogEntry:
      type: object
      required: [type, title, description, default_w, default_h, min_w, min_h, options]
      properties:
        type: { type: string }
        title: { type: string }
        description: { type: string }
        default_w: { type: integer }
        default_h: { type: integer }
        min_w: { type: integer }
        min_h: { type: integer }
        options:
          type: object
          additionalProperties:
            type: object
            properties:
              type: { type: string, enum: [int] }
              min: { type: integer }
              max: { type: integer }
              default: { type: integer }
    DashboardCatalogResponse:
      type: object
      required: [data]
      properties:
        data: { type: array, items: { $ref: '#/components/schemas/DashboardCatalogEntry' } }
```

И два блока путей, после `/api/v1/account/api-keys/{id}`:

```yaml
  # ── Dashboard (owner-scoped: your own layout) ─────────────────────────────
  /api/v1/dashboard/catalog:
    get:
      summary: Widget types you may place, filtered by your permissions
      tags: [dashboard]
      security: [{ BearerAuth: [] }]
      responses:
        '200':
          description: Catalog entries under data[]
          content:
            application/json:
              schema: { $ref: '#/components/schemas/DashboardCatalogResponse' }
        '401': { description: Not authenticated }

  /api/v1/dashboard/layout:
    get:
      summary: Your dashboard layout, ordered by grid position
      tags: [dashboard]
      security: [{ BearerAuth: [] }]
      responses:
        '200':
          description: Widgets under data[] (empty array when nothing is configured)
          content:
            application/json:
              schema: { $ref: '#/components/schemas/DashboardLayoutResponse' }
        '401': { description: Not authenticated }
    put:
      summary: Replace your whole layout (an empty array clears it)
      tags: [dashboard]
      security: [{ BearerAuth: [] }]
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              required: [widgets]
              properties:
                widgets:
                  type: array
                  maxItems: 40
                  items:
                    type: object
                    required: [widget_type, grid_x, grid_y, grid_w, grid_h]
                    properties:
                      widget_type: { type: string }
                      grid_x: { type: integer, minimum: 0, maximum: 11 }
                      grid_y: { type: integer, minimum: 0, maximum: 200 }
                      grid_w: { type: integer, minimum: 1, maximum: 12 }
                      grid_h: { type: integer, minimum: 1, maximum: 24 }
                      options: { type: object, additionalProperties: { type: integer } }
      responses:
        '200':
          description: The stored layout
          content:
            application/json:
              schema: { $ref: '#/components/schemas/DashboardLayoutResponse' }
        '400':
          description: Unknown widget type, geometry outside the grid, or an unknown option
          content:
            application/json:
              schema: { $ref: '#/components/schemas/ValidationError' }
        '401': { description: Not authenticated }
        '403': { description: A widget in the layout needs a permission you lack }
```

- [ ] **Step 4: Run test to verify it passes**

```sh
make test-integration 2>&1 | tail -40
./scripts/check-openapi-drift.sh
./scripts/check-test-buckets.sh
```

Expected: одиннадцать кейсов `DashboardApiTest.*` и пять `DashboardLayoutTest.*` зеленые, оба скрипта без расхождений.

- [ ] **Step 5: Commit**

```bash
git add src/api/DashboardController.hpp src/api/Endpoints.hpp src/api/Api.hpp \
        docs/openapi.yaml tests/integration/test_dashboard_layout.cpp
git commit -m "feat(dashboard): catalog and layout endpoints with owner scoping"
```

---

### Task 4: Tabler в сборке, тема и оболочка

Tailwind НЕ удаляется в этой задаче: он живет до задачи 13, чтобы каждый промежуточный коммит оставался читаемым. Порядок импортов в `main.tsx` ставит Tabler после Tailwind, поэтому reboot Bootstrap выигрывает у preflight.

**Files:**
- Modify: `frontend/package.json`
- Modify: `frontend/src/main.tsx`
- Modify: `frontend/index.html`
- Modify: `frontend/public/theme.js`
- Modify: `frontend/src/lib/brand.ts`
- Modify: `frontend/src/components/Layout.tsx`
- Modify: `frontend/src/components/Nav.tsx`
- Create: `frontend/src/lib/theme.ts`
- Test: `frontend/src/lib/theme.test.ts`

**Interfaces:**
- Produces:
  - `type Theme = 'light' | 'dark'` в `@/lib/theme`
  - `function readTheme(): Theme` — из атрибута `data-bs-theme`, иначе `'dark'`
  - `function applyTheme(next: Theme): void` — ставит атрибут, класс `dark` (для переходного периода Tailwind) и пишет в `localStorage`, проглатывая исключение

- [ ] **Step 1: Write the failing test**

Создать `frontend/src/lib/theme.test.ts`:

```ts
import { describe, expect, it, beforeEach, vi, afterEach } from 'vitest';

import { applyTheme, readTheme } from './theme';

describe('theme', () => {
  beforeEach(() => {
    document.documentElement.removeAttribute('data-bs-theme');
    document.documentElement.classList.remove('dark');
    localStorage.clear();
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('defaults to dark when nothing is set', () => {
    expect(readTheme()).toBe('dark');
  });

  it('reads the attribute the pre-paint script set', () => {
    document.documentElement.setAttribute('data-bs-theme', 'light');
    expect(readTheme()).toBe('light');
  });

  it('applies the attribute, the transitional class and the stored value', () => {
    applyTheme('light');
    expect(document.documentElement.getAttribute('data-bs-theme')).toBe('light');
    expect(document.documentElement.classList.contains('dark')).toBe(false);
    expect(localStorage.getItem('theme')).toBe('light');

    applyTheme('dark');
    expect(document.documentElement.getAttribute('data-bs-theme')).toBe('dark');
    expect(document.documentElement.classList.contains('dark')).toBe(true);
  });

  // Private-mode Safari throws on setItem. The theme must still switch.
  it('survives localStorage throwing', () => {
    vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
      throw new Error('QuotaExceededError');
    });
    expect(() => applyTheme('light')).not.toThrow();
    expect(document.documentElement.getAttribute('data-bs-theme')).toBe('light');
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cd frontend && npm run test 2>&1 | tail -20
```

Expected: `Failed to resolve import "./theme"`.

- [ ] **Step 3: Write minimal implementation**

Установить зависимости:

```sh
cd frontend
npm install @tabler/core@1.6.0 @tabler/icons-react@3.48.0 apexcharts react-apexcharts@2.1.1 \
            @fontsource-variable/inter@5.3.0 react-grid-layout@2.2.4
```

Создать `frontend/src/lib/theme.ts`:

```ts
/**
 * Theme state. Tabler keys its dark palette off `[data-bs-theme="dark"]` on
 * <html>; its own default is LIGHT, so the pre-paint script in /theme.js sets
 * the attribute explicitly to keep this app's dark default.
 *
 * The `dark` class is written too, but only until task 13: the shadcn CSS
 * variables still in index.css key off it, and pages converted to Tabler
 * ignore it. Drop the class once Tailwind is gone.
 */
export type Theme = 'light' | 'dark';

export function readTheme(): Theme {
  return document.documentElement.getAttribute('data-bs-theme') === 'light' ? 'light' : 'dark';
}

export function applyTheme(next: Theme): void {
  document.documentElement.setAttribute('data-bs-theme', next);
  document.documentElement.classList.toggle('dark', next === 'dark');
  try {
    localStorage.setItem('theme', next);
  } catch {
    // Private mode / blocked storage: the theme still switches for this tab.
  }
}
```

Заменить `frontend/public/theme.js`:

```js
// Dark stays this app's default (Tabler's own default is light); only an
// explicit 'light' choice opts out. Runs before first paint to avoid a flash.
// Served as a static /theme.js asset so the production CSP (script-src 'self',
// no inline) covers it.
// The `dark` class is transitional — the shadcn variables in index.css still
// key off it. Remove that line together with Tailwind (task 13).
try {
  var theme = localStorage.getItem('theme') === 'light' ? 'light' : 'dark';
  document.documentElement.setAttribute('data-bs-theme', theme);
  if (theme === 'dark') document.documentElement.classList.add('dark');
} catch (e) {
  document.documentElement.setAttribute('data-bs-theme', 'dark');
  document.documentElement.classList.add('dark');
}
```

В `frontend/index.html` заменить `<title>` и первый-кадр стиль (значения взяты из `@tabler/core@1.6.0`: светлый фон `oklch(98.51% 0 0deg)` это rgb 250 250 250, темный `oklch(20.46% 0 0deg)` это rgb 23 23 23):

```html
    <title>tarassov.me</title>
    <!-- Theme attribute first, then first-paint colors. Order matters: the
         style below keys off [data-bs-theme], so theme.js must run before it. -->
    <script src="/theme.js"></script>
    <style>
      html,
      body {
        margin: 0;
        min-height: 100%;
      }
      html[data-bs-theme='dark'],
      html[data-bs-theme='dark'] body {
        background: #171717;
      }
      html[data-bs-theme='light'],
      html[data-bs-theme='light'] body {
        background: #fafafa;
      }
    </style>
```

В `frontend/src/lib/brand.ts` заменить значение:

```ts
// Single source for the product name shown in the sidebar (and the document
// title in index.html).
export const BRAND = 'tarassov.me';
```

В `frontend/src/main.tsx` добавить импорты стилей ПОСЛЕ `./index.css`:

```ts
import './index.css';
// Tabler last: its reboot must win over Tailwind's preflight while both are in
// the bundle (Tailwind leaves in task 13).
import '@tabler/core/dist/css/tabler.min.css';
import '@fontsource-variable/inter';
```

Заменить `frontend/src/components/Layout.tsx`:

```tsx
import { Outlet } from 'react-router-dom';

import { Nav } from './Nav';
import { useMe } from '@/hooks/useMe';

/**
 * Tabler page shell: a vertical navbar beside a .page-wrapper whose .page-body
 * holds the routed page. Calling useMe here means every page below has a fresh
 * principal in the TanStack Query cache on first paint.
 */
export function Layout() {
  useMe();
  return (
    <div className="page">
      <a href="#main-content" className="visually-hidden-focusable">
        Skip to main content
      </a>
      <Nav />
      <div className="page-wrapper">
        <main id="main-content" className="page-body">
          <Outlet />
        </main>
      </div>
    </div>
  );
}
```

Заменить `frontend/src/components/Nav.tsx`:

```tsx
import { useState } from 'react';
import { Link, useLocation, useNavigate } from 'react-router-dom';
import { IconLogout, IconMoon, IconSun } from '@tabler/icons-react';

import { BRAND } from '@/lib/brand';
import { useLogout } from '@/hooks/useAuthMutations';
import { useMe } from '@/hooks/useMe';
import { applyTheme, readTheme, type Theme } from '@/lib/theme';
import { userCan } from '@/lib/auth/permissions';
import { routes, guardPermission, type RouteEntry } from '@/routes/manifest';

/**
 * Tabler's vertical navbar. The collapse is React state, not Bootstrap JS:
 * the project ships no Bootstrap bundle, so `show` is toggled by hand.
 */
export function Nav() {
  const me = useMe();
  const user = me.data ?? null;
  const logout = useLogout();
  const navigate = useNavigate();
  const location = useLocation();
  const [menuOpen, setMenuOpen] = useState(false);
  const [theme, setTheme] = useState<Theme>(() => readTheme());

  const toggleTheme = () => {
    const next: Theme = theme === 'dark' ? 'light' : 'dark';
    setTheme(next);
    applyTheme(next);
  };

  const showAuthButtons = me.isSuccess && !user;
  const navLinks: RouteEntry[] = routes.filter(
    (r) => r.navLabel && userCan(user, guardPermission(r)),
  );
  const isActive = (path: string) =>
    path === '/' ? location.pathname === '/' : location.pathname.startsWith(path);

  const logoutAndRedirect = async () => {
    await logout.mutateAsync();
    navigate('/login');
  };

  return (
    <aside className="navbar navbar-vertical navbar-expand-lg">
      <div className="container-fluid">
        <button
          className="navbar-toggler"
          type="button"
          aria-label="Toggle navigation menu"
          aria-expanded={menuOpen}
          aria-controls="sidebar-menu"
          onClick={() => setMenuOpen((open) => !open)}
        >
          <span className="navbar-toggler-icon" />
        </button>
        <div className="navbar-brand navbar-brand-autodark">
          <Link to="/admin">{BRAND}</Link>
        </div>
        <div className="navbar-nav flex-row d-lg-none">
          <button
            className="nav-link px-0"
            type="button"
            onClick={toggleTheme}
            aria-label={theme === 'dark' ? 'Switch to light theme' : 'Switch to dark theme'}
          >
            {theme === 'dark' ? <IconSun size={20} /> : <IconMoon size={20} />}
          </button>
        </div>
        <div
          id="sidebar-menu"
          className={menuOpen ? 'collapse navbar-collapse show' : 'collapse navbar-collapse'}
        >
          <ul className="navbar-nav pt-lg-3">
            {navLinks.map((entry) => (
              <li key={entry.path} className={isActive(entry.path) ? 'nav-item active' : 'nav-item'}>
                <Link
                  to={entry.path}
                  className="nav-link"
                  aria-current={isActive(entry.path) ? 'page' : undefined}
                  onClick={() => setMenuOpen(false)}
                >
                  <span className="nav-link-title">{entry.navLabel}</span>
                </Link>
              </li>
            ))}
          </ul>
          <div className="mt-auto pb-3">
            <ul className="navbar-nav">
              <li className="nav-item d-none d-lg-block">
                <button
                  className="nav-link"
                  type="button"
                  onClick={toggleTheme}
                  aria-label={theme === 'dark' ? 'Switch to light theme' : 'Switch to dark theme'}
                >
                  <span className="nav-link-icon">
                    {theme === 'dark' ? <IconSun size={18} /> : <IconMoon size={18} />}
                  </span>
                  <span className="nav-link-title">
                    {theme === 'dark' ? 'Light theme' : 'Dark theme'}
                  </span>
                </button>
              </li>
              {user && (
                <>
                  <li className={isActive('/account') ? 'nav-item active' : 'nav-item'}>
                    <Link to="/account" className="nav-link" onClick={() => setMenuOpen(false)}>
                      <span className="nav-link-title">{user.full_name || user.email}</span>
                    </Link>
                  </li>
                  <li className="nav-item">
                    <button className="nav-link" type="button" onClick={logoutAndRedirect}>
                      <span className="nav-link-icon">
                        <IconLogout size={18} />
                      </span>
                      <span className="nav-link-title">Log out</span>
                    </button>
                  </li>
                </>
              )}
              {showAuthButtons && (
                <>
                  <li className="nav-item">
                    <Link to="/login" className="nav-link" onClick={() => setMenuOpen(false)}>
                      <span className="nav-link-title">Log in</span>
                    </Link>
                  </li>
                  <li className="nav-item">
                    <Link to="/register" className="nav-link" onClick={() => setMenuOpen(false)}>
                      <span className="nav-link-title">Register</span>
                    </Link>
                  </li>
                </>
              )}
            </ul>
          </div>
        </div>
      </div>
    </aside>
  );
}
```

`routes/manifest.tsx` пока импортирует иконки из `lucide-react` для поля `navIcon`. Это поле в новой навигации не используется, но оставить его до задачи 13, чтобы манифест не пришлось трогать дважды.

- [ ] **Step 4: Run test to verify it passes**

```sh
cd frontend && npm run test 2>&1 | tail -20 && npm run typecheck && npm run lint && npm run build
```

Expected: четыре кейса `theme` зеленые, typecheck, lint и сборка без ошибок.

- [ ] **Step 5: Verify in the browser**

```sh
cd frontend && npm run dev
```

Открыть `http://localhost:5173/login`, проверить: боковая панель Tabler на месте, переключатель темы меняет фон, гамбургер на узком окне открывает и закрывает меню. Старые страницы в этот момент выглядят смешанно, это ожидаемо.

- [ ] **Step 6: Commit**

```bash
git add frontend/package.json frontend/package-lock.json frontend/index.html \
        frontend/public/theme.js frontend/src/main.tsx frontend/src/lib/brand.ts \
        frontend/src/lib/theme.ts frontend/src/lib/theme.test.ts \
        frontend/src/components/Layout.tsx frontend/src/components/Nav.tsx
git commit -m "feat(admin): Tabler shell, vertical navbar and data-bs-theme switching"
```

---

### Task 5: Примитивы на Tabler и общие компоненты

Все имена классов ниже сверены с `@tabler/core@1.6.0` (`dist/css/tabler.css`) 28.09.2026: ни одного придуманного.

**Files:**
- Create: `frontend/src/components/tabler/Button.tsx`
- Create: `frontend/src/components/tabler/Card.tsx`
- Create: `frontend/src/components/tabler/Alert.tsx`
- Create: `frontend/src/components/tabler/Input.tsx`
- Create: `frontend/src/components/tabler/Placeholder.tsx`
- Create: `frontend/src/components/tabler/PageHeader.tsx`
- Modify: `frontend/src/components/FormField.tsx`
- Modify: `frontend/src/components/DataTable.tsx`
- Modify: `frontend/src/components/PaginationFooter.tsx`
- Modify: `frontend/src/components/Modal.tsx`
- Modify: `frontend/src/components/ConfirmDialog.tsx`
- Modify: `frontend/src/components/ui/toaster.tsx`
- Test: `frontend/src/components/tabler/Button.test.tsx`

**Interfaces:**
- Produces (эти имена используют все страницы задач 6-12):
  - `<Button variant?: 'primary' | 'secondary' | 'outline' | 'ghost' | 'danger' | 'link'; size?: 'sm' | 'lg' | 'icon'; asChild?: never>` из `@/components/tabler/Button`
  - `<Card>`, `<CardHeader>`, `<CardTitle>`, `<CardBody>`, `<CardFooter>` из `@/components/tabler/Card`
  - `<Alert variant?: 'danger' | 'info' | 'success' | 'warning'>` из `@/components/tabler/Alert`
  - `<Input>`, `<Label>` из `@/components/tabler/Input`
  - `<Placeholder className?: string>` из `@/components/tabler/Placeholder`
  - `<PageHeader title: string; pretitle?: string; actions?: ReactNode>` из `@/components/tabler/PageHeader`

**Карта классов.** Ей пользуются задачи 6-10; держать открытой во время переписи.

| Было (Tailwind) | Стало (Tabler) |
|---|---|
| `container mx-auto py-8` | `container-xl` (вертикальные отступы уже дает `.page-body`) |
| `max-w-md mx-auto`, `max-w-sm` | `container-tight` |
| `flex` / `flex-col` | `d-flex` / `flex-column` |
| `items-center` / `items-start` / `self-center` | `align-items-center` / `align-items-start` / `align-self-center` |
| `justify-between` / `justify-center` | `justify-content-between` / `justify-content-center` |
| `gap-2` / `gap-3` / `gap-4` | те же имена, Bootstrap-утилиты |
| `space-y-N` | `vstack gap-N` на родителе |
| `w-full` | `w-100` |
| `text-sm` | `small` |
| `text-xs` | `small text-secondary` |
| `text-2xl` / `text-3xl` | `h2` / `<PageHeader>` |
| `text-muted-foreground` | `text-secondary` |
| `text-destructive` | `text-danger` |
| `font-bold` / `font-medium` / `font-semibold` / `font-mono` | `fw-bold` / `fw-medium` / `fw-semibold` / `font-monospace` |
| `grid grid-cols-1 sm:grid-cols-2 gap-4` | `row row-cards` плюс `col-sm-6` на элементах |
| `overflow-x-auto` вокруг таблицы | `table-responsive` |
| `border` / `border-b` / `border-border` | `border` / `border-bottom` |
| `rounded` / `rounded-md` | `rounded` |
| `hidden` / `md:hidden` / `md:flex` | `d-none` / `d-md-none` / `d-md-flex` |
| `fixed inset-0 z-50 bg-black/50` | `modal modal-blur fade show d-block` плюс отдельный `modal-backdrop fade show` |
| `hover:underline` | ничего: ссылки Tabler подчеркиваются сами |
| `<Icon className="h-4 w-4" />` | `<Icon size={16} />` у `@tabler/icons-react` |

- [ ] **Step 1: Write the failing test**

Создать `frontend/src/components/tabler/Button.test.tsx`:

```tsx
import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { Button } from './Button';

describe('Button', () => {
  it('defaults to the primary Tabler button', () => {
    render(<Button>Save</Button>);
    expect(screen.getByRole('button', { name: 'Save' }).className).toBe('btn btn-primary');
  });

  it('maps variant and size onto Tabler classes', () => {
    render(
      <Button variant="danger" size="sm">
        Delete
      </Button>,
    );
    const btn = screen.getByRole('button', { name: 'Delete' });
    expect(btn.className).toContain('btn-danger');
    expect(btn.className).toContain('btn-sm');
  });

  it('keeps caller classes and the disabled state', () => {
    render(
      <Button variant="ghost" className="w-100" disabled>
        Next
      </Button>,
    );
    const btn = screen.getByRole('button', { name: 'Next' });
    expect(btn.className).toContain('btn-ghost-secondary');
    expect(btn.className).toContain('w-100');
    expect(btn).toBeDisabled();
  });
});
```

Если `@testing-library/react` и `@testing-library/jest-dom` еще не стоят, поставить их и включить окружение jsdom:

```sh
cd frontend
npm install -D @testing-library/react@16 @testing-library/jest-dom@6 jsdom
```

и добавить в `vite.config.ts` в объект конфигурации:

```ts
  test: {
    environment: 'jsdom',
    setupFiles: ['./src/test-setup.ts'],
  },
```

с файлом `frontend/src/test-setup.ts`:

```ts
import '@testing-library/jest-dom/vitest';
```

Тип `test` в `defineConfig` из `vite` не объявлен, поэтому импорт меняется на `import { defineConfig } from 'vitest/config';`.

- [ ] **Step 2: Run test to verify it fails**

```sh
cd frontend && npm run test 2>&1 | tail -20
```

Expected: `Failed to resolve import "./Button"`.

- [ ] **Step 3: Write minimal implementation**

`frontend/src/components/tabler/Button.tsx`:

```tsx
import * as React from 'react';

import { cn } from '@/lib/utils';

/**
 * Tabler button. No `asChild`: the Radix Slot is gone, so a link that looks
 * like a button is a plain <Link className="btn btn-primary"> at the call site.
 */
export type ButtonVariant = 'primary' | 'secondary' | 'outline' | 'ghost' | 'danger' | 'link';
export type ButtonSize = 'sm' | 'lg' | 'icon';

const VARIANT_CLASS: Record<ButtonVariant, string> = {
  primary: 'btn-primary',
  secondary: 'btn-secondary',
  // Tabler's plain .btn already renders the bordered/outline look.
  outline: '',
  ghost: 'btn-ghost-secondary',
  danger: 'btn-danger',
  link: 'btn-link',
};

const SIZE_CLASS: Record<ButtonSize, string> = {
  sm: 'btn-sm',
  lg: 'btn-lg',
  icon: 'btn-icon',
};

export interface ButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  size?: ButtonSize;
}

export const Button = React.forwardRef<HTMLButtonElement, ButtonProps>(
  ({ className, variant = 'primary', size, type = 'button', ...props }, ref) => (
    <button
      ref={ref}
      type={type}
      className={cn('btn', VARIANT_CLASS[variant], size && SIZE_CLASS[size], className)}
      {...props}
    />
  ),
);
Button.displayName = 'Button';
```

`frontend/src/components/tabler/Card.tsx`:

```tsx
import * as React from 'react';

import { cn } from '@/lib/utils';

export const Card = React.forwardRef<HTMLDivElement, React.HTMLAttributes<HTMLDivElement>>(
  ({ className, ...props }, ref) => <div ref={ref} className={cn('card', className)} {...props} />,
);
Card.displayName = 'Card';

export const CardHeader = React.forwardRef<HTMLDivElement, React.HTMLAttributes<HTMLDivElement>>(
  ({ className, ...props }, ref) => (
    <div ref={ref} className={cn('card-header', className)} {...props} />
  ),
);
CardHeader.displayName = 'CardHeader';

export const CardTitle = React.forwardRef<HTMLHeadingElement, React.HTMLAttributes<HTMLHeadingElement>>(
  ({ className, ...props }, ref) => (
    <h3 ref={ref} className={cn('card-title', className)} {...props} />
  ),
);
CardTitle.displayName = 'CardTitle';

export const CardBody = React.forwardRef<HTMLDivElement, React.HTMLAttributes<HTMLDivElement>>(
  ({ className, ...props }, ref) => (
    <div ref={ref} className={cn('card-body', className)} {...props} />
  ),
);
CardBody.displayName = 'CardBody';

export const CardFooter = React.forwardRef<HTMLDivElement, React.HTMLAttributes<HTMLDivElement>>(
  ({ className, ...props }, ref) => (
    <div ref={ref} className={cn('card-footer', className)} {...props} />
  ),
);
CardFooter.displayName = 'CardFooter';
```

`frontend/src/components/tabler/Alert.tsx`:

```tsx
import * as React from 'react';

import { cn } from '@/lib/utils';

export type AlertVariant = 'danger' | 'info' | 'success' | 'warning';

export interface AlertProps extends React.HTMLAttributes<HTMLDivElement> {
  variant?: AlertVariant;
}

export const Alert = React.forwardRef<HTMLDivElement, AlertProps>(
  ({ className, variant = 'info', role = 'alert', ...props }, ref) => (
    <div ref={ref} role={role} className={cn('alert', `alert-${variant}`, className)} {...props} />
  ),
);
Alert.displayName = 'Alert';
```

`frontend/src/components/tabler/Input.tsx`:

```tsx
import * as React from 'react';

import { cn } from '@/lib/utils';

export interface InputProps extends React.InputHTMLAttributes<HTMLInputElement> {
  /** Renders Bootstrap's invalid state, which the .invalid-feedback text pairs with. */
  invalid?: boolean;
}

export const Input = React.forwardRef<HTMLInputElement, InputProps>(
  ({ className, invalid, ...props }, ref) => (
    <input ref={ref} className={cn('form-control', invalid && 'is-invalid', className)} {...props} />
  ),
);
Input.displayName = 'Input';

export const Label = React.forwardRef<HTMLLabelElement, React.LabelHTMLAttributes<HTMLLabelElement>>(
  ({ className, ...props }, ref) => (
    <label ref={ref} className={cn('form-label', className)} {...props} />
  ),
);
Label.displayName = 'Label';
```

`frontend/src/components/tabler/Placeholder.tsx`:

```tsx
import { cn } from '@/lib/utils';

/**
 * Loading bar. Tabler's .placeholder needs a .placeholder-glow ancestor to
 * animate, so the wrapper is part of the component rather than a call-site
 * detail everyone forgets.
 */
export function Placeholder({ className }: { className?: string }) {
  return (
    <span className="placeholder-glow">
      <span className={cn('placeholder', className ?? 'col-12')} />
    </span>
  );
}
```

`frontend/src/components/tabler/PageHeader.tsx`:

```tsx
import type { ReactNode } from 'react';

/**
 * The page-header block from the Tabler preview: pretitle, title and a right
 * aligned action slot. Every admin page opens with one, so the markup lives
 * here instead of being retyped 9 times.
 */
export function PageHeader({
  title,
  pretitle,
  actions,
}: {
  title: string;
  pretitle?: string;
  actions?: ReactNode;
}) {
  return (
    <div className="page-header d-print-none">
      <div className="container-xl">
        <div className="row align-items-center">
          <div className="col">
            {pretitle && <div className="page-pretitle">{pretitle}</div>}
            <h2 className="page-title">{title}</h2>
          </div>
          {actions && <div className="col-auto ms-auto d-print-none">{actions}</div>}
        </div>
      </div>
    </div>
  );
}
```

Переписать `frontend/src/components/FormField.tsx` (сигнатура пропсов не меняется, чтобы страницы не трогать дважды):

```tsx
import { forwardRef, type InputHTMLAttributes } from 'react';

import { Input, Label } from '@/components/tabler/Input';

interface FormFieldProps extends InputHTMLAttributes<HTMLInputElement> {
  id: string;
  label: string;
  /** Validation message (usually `errors.<field>?.message` from RHF). */
  error?: string;
}

export const FormField = forwardRef<HTMLInputElement, FormFieldProps>(
  ({ id, label, error, ...inputProps }, ref) => (
    <div className="mb-3">
      <Label htmlFor={id}>{label}</Label>
      <Input
        id={id}
        ref={ref}
        invalid={!!error}
        aria-invalid={!!error}
        aria-describedby={error ? `${id}-error` : undefined}
        {...inputProps}
      />
      {error && (
        // d-block because Bootstrap only reveals .invalid-feedback next to a
        // .is-invalid sibling in a form-validated <form>; we drive it ourselves.
        <div id={`${id}-error`} role="alert" className="invalid-feedback d-block">
          {error}
        </div>
      )}
    </div>
  ),
);
FormField.displayName = 'FormField';
```

`frontend/src/components/DataTable.tsx`: заменить импорт `Skeleton` на `Placeholder` из `@/components/tabler/Placeholder` и классы по карте:

- корневой `<table className="w-full text-sm">` → `<table className="table table-vcenter card-table">`;
- `<thead><tr className="border-b …">` → `<thead><tr>` (Tabler сам оформляет шапку);
- `<th className="py-1.5 pr-4 font-medium …">` → `<th className={c.className}>`;
- `<td className="py-1.5 pr-4 …">` → `<td className={c.className}>`;
- строка-скелет: `<Skeleton className="h-4 w-full" />` → `<Placeholder />`;
- `isPlaceholder ? 'opacity-50' : ''` → `isPlaceholder ? 'opacity-50' : ''` заменить на `isPlaceholder ? 'opacity-75' : ''` и класс `opacity-75` оставить bootstrap-овским;
- пустое состояние `<p className="text-muted-foreground">` → `<p className="text-secondary">`;
- ошибка `<p className="text-destructive">` → `<p className="text-danger">`.

`frontend/src/components/PaginationFooter.tsx`: импорт `Button` из `@/components/tabler/Button`, обертка `className="mt-4 flex items-center justify-between"` → `className="mt-3 d-flex align-items-center justify-content-between"`, `<p className="text-sm text-muted-foreground">` → `<p className="small text-secondary mb-0">`, `<div className="space-x-2">` → `<div className="btn-list">`, у обеих кнопок `variant="outline"`.

`frontend/src/components/Modal.tsx` и `ConfirmDialog.tsx`: заменить оверлей. Вместо `fixed inset-0 z-50 … bg-black/50`:

```tsx
    <>
      <div className="modal-backdrop fade show" />
      <div
        className="modal modal-blur fade show d-block"
        tabIndex={-1}
        onMouseDown={(e) => {
          pressedBackdrop.current = e.target === e.currentTarget;
        }}
        onClick={(e) => {
          const dismiss = pressedBackdrop.current && e.target === e.currentTarget;
          pressedBackdrop.current = false;
          if (dismiss) requestClose();
        }}
      >
        <div
          className={cn('modal-dialog modal-dialog-centered', className)}
          role="document"
          onClick={(e) => e.stopPropagation()}
        >
          <div ref={ref} role="dialog" aria-modal="true" tabIndex={-1} className="modal-content">
            {children}
          </div>
        </div>
      </div>
    </>
```

Логику `pressedBackdrop`, `requestClose` и `useFocusTrap` не трогать: она про поведение, а не про тему. В `ConfirmDialog` заголовок `text-lg font-semibold` → `modal-title`, описание `text-sm text-muted-foreground` → `text-secondary`, кнопки в `<div className="modal-footer">`, разрушающая через `variant="danger"`.

`frontend/src/components/ui/toaster.tsx`: перенести в `frontend/src/components/tabler/Toaster.tsx`, контейнер `toast-container position-fixed bottom-0 end-0 p-3`, каждый тост `toast show`, тело `toast-body`, закрытие `btn-close`. Публичный API (`ToastProvider`, хук) не менять, иначе придется править `main.tsx` и страницы.

- [ ] **Step 4: Run test to verify it passes**

```sh
cd frontend && npm run test 2>&1 | tail -20 && npm run typecheck && npm run lint
```

Expected: три кейса `Button` зеленые, остальные тесты не сломались.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/components frontend/src/test-setup.ts frontend/vite.config.ts \
        frontend/package.json frontend/package-lock.json
git commit -m "feat(admin): Tabler primitives and shared components"
```

---

### Task 6: Мелкие страницы

Семь файлов по 18-42 строки, механическая замена по карте классов из задачи 5.

**Files:**
- Modify: `frontend/src/pages/NotFound.tsx`, `ConfirmEmail.tsx`, `ConfirmChangeEmail.tsx`, `About.tsx`, `Home.tsx`, `CheckEmail.tsx`, `Profile.tsx`

**Interfaces:**
- Consumes: `Button`, `Card`/`CardBody`, `Alert`, `PageHeader` из `@/components/tabler/*` (задача 5).
- Produces: ничего нового.

`Home.tsx` в проде недостижим (на `/` nginx отдает визитку), поэтому переносится как есть, без переделки содержания.

- [ ] **Step 1: Convert the first page as the worked example**

`frontend/src/pages/NotFound.tsx` целиком:

```tsx
import { Link } from 'react-router-dom';

export function NotFoundPage() {
  return (
    <div className="container-tight py-4">
      <div className="empty">
        <div className="empty-header">404</div>
        <p className="empty-title">Page not found</p>
        <p className="empty-subtitle text-secondary">
          The page you are looking for does not exist.
        </p>
        <div className="empty-action">
          <Link to="/" className="btn btn-primary">
            Go home
          </Link>
        </div>
      </div>
    </div>
  );
}
```

- [ ] **Step 2: Convert the remaining six**

Пройти `ConfirmEmail.tsx`, `ConfirmChangeEmail.tsx`, `About.tsx`, `Home.tsx`, `CheckEmail.tsx`, `Profile.tsx` по карте классов. Правила, которые покрывают эти файлы целиком:

1. Внешний `<div className="container mx-auto py-8 …">` → `<div className="container-xl">`, а для узких карточек `<div className="container-tight py-4">`.
2. `space-y-N` на контейнере → `vstack gap-N`.
3. Заголовок страницы `<h1 className="text-3xl font-bold">X</h1>` → `<PageHeader title="X" />` вынести над контентом.
4. Импорты `Card`, `CardContent`, `Button`, `Alert` перевести на `@/components/tabler/*`, `CardContent` переименовать в `CardBody`.
5. Иконки `lucide-react` заменить на одноименные из `@tabler/icons-react` с префиксом `Icon` и пропом `size` вместо классов.

- [ ] **Step 3: Verify**

```sh
cd frontend && npm run typecheck && npm run lint && npm run test 2>&1 | tail -10
grep -rn 'lucide-react\|text-muted-foreground\|space-y-' src/pages/NotFound.tsx src/pages/ConfirmEmail.tsx \
  src/pages/ConfirmChangeEmail.tsx src/pages/About.tsx src/pages/Home.tsx src/pages/CheckEmail.tsx src/pages/Profile.tsx
```

Expected: typecheck, lint и тесты зеленые, `grep` ничего не находит.

- [ ] **Step 4: Look at them**

`npm run dev`, открыть `/account`, `/about` и несуществующий путь. Карточки, отступы и типографика от Tabler, обе темы читаются.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/pages/NotFound.tsx frontend/src/pages/ConfirmEmail.tsx \
        frontend/src/pages/ConfirmChangeEmail.tsx frontend/src/pages/About.tsx \
        frontend/src/pages/Home.tsx frontend/src/pages/CheckEmail.tsx frontend/src/pages/Profile.tsx
git commit -m "refactor(admin): convert the small pages to Tabler markup"
```

---

### Task 7: Страницы форм

Восемь файлов на `react-hook-form` с zod. Схемы валидации и мутации не трогаются: меняется только разметка вокруг `<FormField>`, который уже переведен в задаче 5.

**Files:**
- Modify: `frontend/src/pages/Login.tsx`, `Register.tsx`, `JoinFromInvite.tsx`, `RequestReset.tsx`, `ResetPassword.tsx`, `ChangeEmail.tsx`, `ChangePassword.tsx`, `Unconfirmed.tsx`

**Interfaces:**
- Consumes: `FormField` (без изменения пропсов), `Button`, `Card`, `CardBody`, `CardTitle`, `Alert` из `@/components/tabler/*`.
- Produces: ничего нового.

- [ ] **Step 1: Convert Login.tsx as the worked example**

Каркас, к которому приводятся все восемь (тело формы и хуки взять из текущего файла без изменений):

```tsx
  return (
    <div className="container-tight py-4">
      <Card>
        <CardBody>
          <CardTitle className="mb-4">Log in</CardTitle>
          {error && (
            <Alert variant="danger" className="mb-3">
              {error}
            </Alert>
          )}
          <form onSubmit={handleSubmit(onSubmit)} noValidate>
            <FormField
              id="email"
              type="email"
              label="Email"
              autoComplete="email"
              error={errors.email?.message}
              {...register('email')}
            />
            <FormField
              id="password"
              type="password"
              label="Password"
              autoComplete="current-password"
              error={errors.password?.message}
              {...register('password')}
            />
            <div className="form-footer">
              <Button type="submit" className="w-100" disabled={isSubmitting}>
                {isSubmitting ? 'Signing in…' : 'Log in'}
              </Button>
            </div>
          </form>
        </CardBody>
      </Card>
      <div className="text-center text-secondary mt-3">
        <Link to="/account/reset-password">Forgot password?</Link>
      </div>
    </div>
  );
```

- [ ] **Step 2: Convert the remaining seven**

Те же правила: внешняя обертка `container-tight py-4`, карточка `Card`/`CardBody`, ошибки через `<Alert variant="danger">`, submit-кнопка в `<div className="form-footer">` с `className="w-100"`, вспомогательные ссылки под карточкой в `text-center text-secondary mt-3`. `space-y-*` между полями больше не нужен: `<FormField>` сам несет `mb-3`.

- [ ] **Step 3: Verify**

```sh
cd frontend && npm run typecheck && npm run lint && npm run test 2>&1 | tail -10
```

Expected: зеленые, включая `JoinFromInvite.test.ts` (он про логику токена, разметку не трогает).

- [ ] **Step 4: Look at them**

`npm run dev`, пройти `/login`, `/register`, `/account/reset-password`. Проверить, что ошибка валидации показывается под полем красным (`is-invalid` плюс `invalid-feedback`), а не пропадает.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/pages/Login.tsx frontend/src/pages/Register.tsx \
        frontend/src/pages/JoinFromInvite.tsx frontend/src/pages/RequestReset.tsx \
        frontend/src/pages/ResetPassword.tsx frontend/src/pages/ChangeEmail.tsx \
        frontend/src/pages/ChangePassword.tsx frontend/src/pages/Unconfirmed.tsx
git commit -m "refactor(admin): convert the auth and account forms to Tabler"
```

---

### Task 8: Табличные страницы админки

**Files:**
- Modify: `frontend/src/pages/admin/Users.tsx`, `UserDetail.tsx`, `InviteUser.tsx`, `Media.tsx`

**Interfaces:**
- Consumes: `DataTable`, `PaginationFooter` (переведены в задаче 5), `PageHeader`, `Card`, `CardBody`, `Button`, `ConfirmDialog`.
- Produces: ничего нового.

- [ ] **Step 1: Convert Users.tsx as the worked example**

Каркас списочной страницы, к которому приводятся все четыре (получение данных, колонки и мутации берутся из текущего файла):

```tsx
  return (
    <>
      <PageHeader
        title="Users"
        pretitle="Admin"
        actions={
          <Link to="/admin/invite" className="btn btn-primary">
            Invite user
          </Link>
        }
      />
      <div className="container-xl">
        <Card>
          <div className="table-responsive">
            <DataTable
              columns={columns}
              rows={query.data?.data}
              rowKey={(u) => u.id}
              isLoading={query.isLoading}
              error={query.error}
              isPlaceholder={query.isPlaceholderData}
              emptyText="No users yet."
            />
          </div>
          <CardFooter>
            <PaginationFooter
              page={page}
              totalPages={totalPages}
              isPlaceholderData={query.isPlaceholderData}
              onPageChange={setPage}
            />
          </CardFooter>
        </Card>
      </div>
    </>
  );
```

- [ ] **Step 2: Convert the remaining three**

`UserDetail.tsx` и `InviteUser.tsx`: `<PageHeader title="…" pretitle="Admin" />`, затем `<div className="container-xl">` с `<Card><CardBody>`, поля через `<FormField>` (он сам несет `mb-3`), ошибки через `<Alert variant="danger" className="mb-3">`, кнопка отправки в `<div className="form-footer">`. Разрушающие действия через `<Button variant="danger">` и существующий `ConfirmDialog`.

`Media.tsx`: сетка превью `grid grid-cols-1 sm:grid-cols-2 …` → `<div className="row row-cards">` с `<div className="col-sm-6 col-lg-3">` на каждом элементе, внутри `<Card className="card-sm"><CardBody>`. Кнопка загрузки в `actions` у `PageHeader`.

- [ ] **Step 3: Verify**

```sh
cd frontend && npm run typecheck && npm run lint && npm run test 2>&1 | tail -10
```

- [ ] **Step 4: Look at them**

`npm run dev`, открыть `/admin/users`, `/admin/users/<id>`, `/admin/invite`, `/admin/media`. Проверить пагинацию, диалог удаления и загрузку картинки.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/pages/admin/Users.tsx frontend/src/pages/admin/UserDetail.tsx \
        frontend/src/pages/admin/InviteUser.tsx frontend/src/pages/admin/Media.tsx
git commit -m "refactor(admin): convert the user and media pages to Tabler"
```

---

### Task 9: Тяжелые страницы админки

`Audit.tsx` (267 строк), `Roles.tsx` (294), `Jobs.tsx` (316). У всех трех фильтры, пагинация и диалоги, поэтому идут отдельной задачей и отдельным ревью.

**Files:**
- Modify: `frontend/src/pages/admin/Audit.tsx`, `Roles.tsx`, `Jobs.tsx`

**Interfaces:**
- Consumes: то же, что задача 8, плюс `Modal` и `ConfirmDialog`.
- Produces: ничего нового.

- [ ] **Step 1: Convert Audit.tsx**

Шапка и таблица как у остальных списков: `<PageHeader title="Audit" pretitle="Admin" />`, `<div className="container-xl">`, `<Card>` с `<div className="table-responsive">` вокруг `<DataTable>` и `<CardFooter>` с `<PaginationFooter>`. Над таблицей появляется карточка фильтров:

```tsx
      <div className="container-xl">
        <Card className="mb-3">
          <CardBody>
            <div className="row g-2 align-items-end">
              <div className="col-md-3">
                <Label htmlFor="filter-action">Action</Label>
                <Input id="filter-action" value={action} onChange={(e) => setAction(e.target.value)} />
              </div>
              <div className="col-md-3">
                <Label htmlFor="filter-actor">Actor</Label>
                <Input id="filter-actor" value={actor} onChange={(e) => setActor(e.target.value)} />
              </div>
              <div className="col-auto">
                <Button variant="outline" onClick={resetFilters}>
                  Reset
                </Button>
              </div>
            </div>
          </CardBody>
        </Card>
        {/* ниже Card с table-responsive вокруг DataTable и CardFooter с PaginationFooter */}
      </div>
```

Поля `<select>` переводятся на `className="form-select"`, чекбоксы на `form-check` с `form-check-input`.

- [ ] **Step 2: Convert Roles.tsx**

Битовая матрица прав ложится на `form-check` в `row`. Модальное окно создания и правки роли уже переведено на `modal-content` в задаче 5, внутри нужны `modal-header` с `modal-title`, `modal-body` и `modal-footer`.

- [ ] **Step 3: Convert Jobs.tsx**

Статусы вместо цветных `span` на Tailwind переводятся на `badge`: `bg-green` для успеха, `bg-yellow` для ожидания, `bg-red` для DLQ. Список оборачивается так же, как остальные: `<PageHeader title="Jobs" pretitle="Admin" />`, `<div className="container-xl">`, `<Card>` с `<div className="table-responsive">` вокруг `<DataTable>` и `<CardFooter>` с `<PaginationFooter>`.

- [ ] **Step 4: Verify**

```sh
cd frontend && npm run typecheck && npm run lint && npm run test 2>&1 | tail -10
```

- [ ] **Step 5: Look at them**

`npm run dev`, открыть `/admin/audit`, `/admin/roles`, `/admin/jobs`. Проверить: фильтры применяются, роль создается и удаляется, джоба возвращается из DLQ.

- [ ] **Step 6: Commit**

```bash
git add frontend/src/pages/admin/Audit.tsx frontend/src/pages/admin/Roles.tsx frontend/src/pages/admin/Jobs.tsx
git commit -m "refactor(admin): convert the audit, roles and jobs pages to Tabler"
```

---

### Task 10: Страница постов

554 строки и единственная точка, где ошибка видна публично. Отдельная задача, отдельный коммит, ручная проверка на черновике и на публикации.

**Files:**
- Modify: `frontend/src/pages/admin/Posts.tsx`

**Interfaces:**
- Consumes: `PageHeader`, `Card`, `CardBody`, `CardFooter`, `Button`, `Input`, `Label`, `Alert`, `Modal`, `ConfirmDialog`, `DataTable`, `PaginationFooter`.
- Produces: ничего нового.

- [ ] **Step 1: Convert the list**

Список, фильтры и пагинация по каркасу задачи 8. Статус поста в колонке через `badge`: `bg-green` для `published`, `bg-secondary` для `draft`.

- [ ] **Step 2: Convert the editor**

Редактор внутри `Modal` (уже `modal-content`): `modal-header` с заголовком, `modal-body` с полями, `modal-footer` с кнопками. Поле тела поста `<textarea className="form-control" rows={18}>`, поля slug и title через `FormField`, загрузка картинки кнопкой `btn` рядом с полем.

- [ ] **Step 3: Verify**

```sh
cd frontend && npm run typecheck && npm run lint && npm run test 2>&1 | tail -10 && npm run build
```

- [ ] **Step 4: Check the blog end to end**

```sh
make up
cd frontend && npm run dev
```

1. Создать черновик, проверить превью черновика.
2. Загрузить картинку и увидеть ее в теле поста.
3. Опубликовать пост.
4. Открыть `http://localhost:8080/blog.html` и `http://localhost:8080/blog/<slug>`: пост на месте, картинка грузится, разметка визитки не поехала.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/pages/admin/Posts.tsx
git commit -m "refactor(admin): convert the posts page and editor to Tabler"
```

---

### Task 11: Клиент раскладки, реестр и пять виджетов

**Files:**
- Create: `frontend/src/lib/api/dashboard.ts`
- Create: `frontend/src/hooks/useDashboardLayout.ts`
- Create: `frontend/src/widgets/registry.tsx`
- Create: `frontend/src/widgets/PostsSummary.tsx`, `JobsQueue.tsx`, `AuditRecent.tsx`, `UsersRecent.tsx`, `ServiceHealth.tsx`
- Modify: `frontend/src/lib/api/queryKeys.ts`
- Test: `frontend/src/widgets/registry.test.ts`

**Interfaces:**
- Consumes: `GET /api/v1/dashboard/catalog`, `GET/PUT /api/v1/dashboard/layout` (задача 3); `api` из `@/lib/api/client`; `useApiMutation`.
- Produces:
  - `interface DashboardWidget { id: string; widget_type: string; grid_x: number; grid_y: number; grid_w: number; grid_h: number; options: Record<string, number>; created_at: string; updated_at: string }`
  - `interface CatalogEntry { type: string; title: string; description: string; default_w: number; default_h: number; min_w: number; min_h: number; options: Record<string, { type: 'int'; min: number; max: number; default: number }> }`
  - `interface WidgetPlacement { widget_type: string; grid_x: number; grid_y: number; grid_w: number; grid_h: number; options?: Record<string, number> }`
  - `fetchLayout(): Promise<DashboardWidget[]>`, `fetchCatalog(): Promise<CatalogEntry[]>`, `saveLayout(widgets: WidgetPlacement[]): Promise<DashboardWidget[]>`
  - `useDashboardLayout()` → `{ layout, catalog, isLoading, error, save, isSaving, saveError }`, где `save(widgets: WidgetPlacement[]): void` дебаунсит 800 мс
  - `WIDGET_COMPONENTS: Record<string, React.ComponentType<{ options: Record<string, number> }>>` в `@/widgets/registry`
  - `qk.dashboard.layout()` и `qk.dashboard.catalog()`

- [ ] **Step 1: Write the failing test**

Создать `frontend/src/widgets/registry.test.ts`:

```ts
import { describe, expect, it } from 'vitest';

import { WIDGET_COMPONENTS } from './registry';

/**
 * The backend catalog is the single source of widget types. This test pins the
 * frontend registry to it: a type added in Domain::Widgets::kCatalog without a
 * component here would render an empty card in production.
 */
const BACKEND_TYPES = [
  'posts_summary',
  'jobs_queue',
  'audit_recent',
  'users_recent',
  'service_health',
] as const;

describe('widget registry', () => {
  it('has a component for every catalog type', () => {
    for (const type of BACKEND_TYPES) {
      expect(WIDGET_COMPONENTS[type], `missing component for ${type}`).toBeDefined();
    }
  });

  it('has no component without a catalog entry', () => {
    expect(Object.keys(WIDGET_COMPONENTS).sort()).toEqual([...BACKEND_TYPES].sort());
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cd frontend && npm run test 2>&1 | tail -20
```

Expected: `Failed to resolve import "./registry"`.

- [ ] **Step 3: Write minimal implementation**

Обновить типы из спеки:

```sh
cd frontend && npm run gen:api
```

Создать `frontend/src/lib/api/dashboard.ts`:

```ts
import { api } from '@/lib/api/client';

/** One placed widget as the backend stores it. */
export interface DashboardWidget {
  id: string;
  widget_type: string;
  grid_x: number;
  grid_y: number;
  grid_w: number;
  grid_h: number;
  options: Record<string, number>;
  created_at: string;
  updated_at: string;
}

/** One type the caller may place, with its geometry defaults and option bounds. */
export interface CatalogEntry {
  type: string;
  title: string;
  description: string;
  default_w: number;
  default_h: number;
  min_w: number;
  min_h: number;
  options: Record<string, { type: 'int'; min: number; max: number; default: number }>;
}

/** What PUT accepts: no id, the server mints one per placement. */
export interface WidgetPlacement {
  widget_type: string;
  grid_x: number;
  grid_y: number;
  grid_w: number;
  grid_h: number;
  options?: Record<string, number>;
}

export async function fetchLayout(): Promise<DashboardWidget[]> {
  const body = await api.getJson<{ data: DashboardWidget[] }>('/api/v1/dashboard/layout');
  return body.data;
}

export async function fetchCatalog(): Promise<CatalogEntry[]> {
  const body = await api.getJson<{ data: CatalogEntry[] }>('/api/v1/dashboard/catalog');
  return body.data;
}

export async function saveLayout(widgets: WidgetPlacement[]): Promise<DashboardWidget[]> {
  const body = await api.putJson<{ data: DashboardWidget[] }>('/api/v1/dashboard/layout', {
    widgets,
  });
  return body.data;
}
```

Клиент типизирован деревом `paths` из `schema.gen.ts`, а маршруты дашборда попали туда на шаге `npm run gen:api` выше. Поэтому явный generic у `getJson` не нужен: типизированная перегрузка выводит тело ответа сама. Проверить фактический набор хелперов и убрать generic там, где перегрузка его не принимает:

```sh
grep -n 'getJson\|postJson\|patchJson\|putJson\|deleteJson' src/lib/api/client.ts | head
```

Если `putJson` в клиенте нет, добавить его рядом с `patchJson` по тому же образцу: одна строка поверх `api.PUT`. Второго способа ходить в API в проекте быть не должно.

Дописать в `frontend/src/lib/api/queryKeys.ts` внутрь объекта `qk`:

```ts
  dashboard: {
    layout: () => ['dashboard', 'layout'] as const,
    catalog: () => ['dashboard', 'catalog'] as const,
  },
```

Создать `frontend/src/hooks/useDashboardLayout.ts`:

```ts
import { useCallback, useEffect, useRef } from 'react';
import { useQuery } from '@tanstack/react-query';

import { fetchCatalog, fetchLayout, saveLayout, type WidgetPlacement } from '@/lib/api/dashboard';
import { qk } from '@/lib/api/queryKeys';
import { useApiMutation } from '@/hooks/useApiMutation';

/** Debounce for layout writes: dragging fires continuously, the server needs one PUT. */
const SAVE_DELAY_MS = 800;

/**
 * Reads the layout and the catalog, and writes the layout back debounced.
 * Only the arrangement at the END of a drag is worth a request, so `save`
 * coalesces calls and the timer is cleared on unmount — a pending write must
 * not fire after the page is gone.
 */
export function useDashboardLayout() {
  const layout = useQuery({ queryKey: qk.dashboard.layout(), queryFn: fetchLayout });
  const catalog = useQuery({ queryKey: qk.dashboard.catalog(), queryFn: fetchCatalog });

  const mutation = useApiMutation(saveLayout, { invalidate: [qk.dashboard.layout()] });

  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const pending = useRef<WidgetPlacement[] | null>(null);
  // The mutation object is new on every render; a ref keeps `save` stable so a
  // consumer can pass it straight to onLayoutChange without re-subscribing.
  const mutateRef = useRef(mutation.mutate);
  mutateRef.current = mutation.mutate;

  const save = useCallback((widgets: WidgetPlacement[]) => {
    pending.current = widgets;
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(() => {
      timer.current = null;
      if (pending.current) mutateRef.current(pending.current);
      pending.current = null;
    }, SAVE_DELAY_MS);
  }, []);

  useEffect(
    () => () => {
      if (timer.current) clearTimeout(timer.current);
    },
    [],
  );

  return {
    layout: layout.data,
    catalog: catalog.data,
    isLoading: layout.isLoading || catalog.isLoading,
    error: layout.error ?? catalog.error,
    save,
    isSaving: mutation.isPending,
    saveError: mutation.error,
  };
}
```

Создать пять виджетов. Каждый принимает `options` и сам тянет свои данные существующей ручкой. Образец, `frontend/src/widgets/PostsSummary.tsx`:

```tsx
import { useQuery } from '@tanstack/react-query';
import { Link } from 'react-router-dom';

import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import { Placeholder } from '@/components/tabler/Placeholder';

interface PostRow {
  id: string;
  title: string;
  slug: string;
  status: string;
}

export function PostsSummary({ options }: { options: Record<string, number> }) {
  const limit = options.limit ?? 5;
  const query = useQuery({
    queryKey: qk.admin.posts(`widget:${limit}`),
    queryFn: () =>
      api.getJson<{ data: PostRow[]; total: number }>(`/api/v1/posts?limit=${limit}&offset=0`),
  });

  if (query.isLoading) return <Placeholder />;
  if (query.error) return <p className="text-danger mb-0">Failed to load posts.</p>;

  const rows = query.data?.data ?? [];
  const published = rows.filter((p) => p.status === 'published').length;

  return (
    <>
      <div className="d-flex align-items-baseline gap-3 mb-3">
        <span className="h1 mb-0">{query.data?.total ?? 0}</span>
        <span className="text-secondary small">
          {published} published in the latest {rows.length}
        </span>
      </div>
      <div className="list-group list-group-flush">
        {rows.map((p) => (
          <Link key={p.id} to="/admin/posts" className="list-group-item list-group-item-action">
            <span className="text-truncate d-block">{p.title}</span>
            <span className="small text-secondary">{p.status}</span>
          </Link>
        ))}
      </div>
    </>
  );
}
```

Остальные четыре по той же форме:

- `JobsQueue.tsx`: берет `/api/v1/jobs` и `/api/v1/jobs/dlq`, рисует распределение по статусам через `react-apexcharts` (тип `donut`), под графиком строка «DLQ: N». Опция `window_days` уходит в параметр запроса, если ручка его принимает, иначе фильтрует на клиенте по `created_at`.
- `AuditRecent.tsx`: `/api/v1/admin/audit?limit=<limit>` в `table table-vcenter card-table` на три колонки (время, действие, актор).
- `UsersRecent.tsx`: `/api/v1/admin/users?limit=<limit>` плюс `total` крупной цифрой.
- `ServiceHealth.tsx`: `/api/v1/health`, статус через `badge` (`bg-green` при `ok`, `bg-red` иначе) и версия строкой.

Создать `frontend/src/widgets/registry.tsx`:

```tsx
import type { ComponentType } from 'react';

import { AuditRecent } from './AuditRecent';
import { JobsQueue } from './JobsQueue';
import { PostsSummary } from './PostsSummary';
import { ServiceHealth } from './ServiceHealth';
import { UsersRecent } from './UsersRecent';

export type WidgetComponent = ComponentType<{ options: Record<string, number> }>;

/**
 * Widget type → component. The KEYS must match Domain::Widgets::kCatalog on the
 * backend exactly; registry.test.ts pins that, because a type without a
 * component here renders an empty card and nothing else complains.
 */
export const WIDGET_COMPONENTS: Record<string, WidgetComponent> = {
  posts_summary: PostsSummary,
  jobs_queue: JobsQueue,
  audit_recent: AuditRecent,
  users_recent: UsersRecent,
  service_health: ServiceHealth,
};
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cd frontend && npm run test 2>&1 | tail -20 && npm run typecheck && npm run lint
```

Expected: два кейса `widget registry` зеленые.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/lib/api/dashboard.ts frontend/src/lib/api/queryKeys.ts \
        frontend/src/lib/api/schema.gen.ts frontend/src/hooks/useDashboardLayout.ts frontend/src/widgets
git commit -m "feat(dashboard): layout client, widget registry and the first five widgets"
```

---

### Task 12: Главная с сеткой виджетов

**Files:**
- Modify: `frontend/src/pages/admin/Dashboard.tsx`
- Create: `frontend/src/pages/admin/WidgetPicker.tsx`
- Test: `frontend/src/pages/admin/dashboardLayout.test.ts`
- Create: `frontend/src/pages/admin/dashboardLayout.ts`

**Interfaces:**
- Consumes: `useDashboardLayout`, `WIDGET_COMPONENTS`, типы из `@/lib/api/dashboard` (задача 11).
- Produces:
  - `toGridItems(widgets: DashboardWidget[]): Layout[]` в `@/pages/admin/dashboardLayout` (`Layout` из `react-grid-layout`)
  - `toPlacements(items: Layout[], byKey: Map<string, DashboardWidget>): WidgetPlacement[]`

- [ ] **Step 1: Write the failing test**

Создать `frontend/src/pages/admin/dashboardLayout.test.ts`:

```ts
import { describe, expect, it } from 'vitest';

import type { DashboardWidget } from '@/lib/api/dashboard';

import { toGridItems, toPlacements } from './dashboardLayout';

function widget(over: Partial<DashboardWidget> = {}): DashboardWidget {
  return {
    id: 'w1',
    widget_type: 'posts_summary',
    grid_x: 0,
    grid_y: 0,
    grid_w: 4,
    grid_h: 3,
    options: {},
    created_at: '2026-09-28T00:00:00Z',
    updated_at: '2026-09-28T00:00:00Z',
    ...over,
  };
}

describe('dashboard layout mapping', () => {
  it('maps stored widgets onto grid items keyed by id', () => {
    const items = toGridItems([widget(), widget({ id: 'w2', grid_x: 4, grid_w: 6 })]);
    expect(items).toEqual([
      { i: 'w1', x: 0, y: 0, w: 4, h: 3 },
      { i: 'w2', x: 4, y: 0, w: 6, h: 3 },
    ]);
  });

  it('maps grid items back to placements, carrying options over', () => {
    const stored = new Map([['w1', widget({ options: { limit: 7 } })]]);
    const placements = toPlacements([{ i: 'w1', x: 2, y: 1, w: 6, h: 4 }], stored);
    expect(placements).toEqual([
      {
        widget_type: 'posts_summary',
        grid_x: 2,
        grid_y: 1,
        grid_w: 6,
        grid_h: 4,
        options: { limit: 7 },
      },
    ]);
  });

  it('drops grid items whose widget is gone instead of sending a bad type', () => {
    expect(toPlacements([{ i: 'ghost', x: 0, y: 0, w: 4, h: 3 }], new Map())).toEqual([]);
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cd frontend && npm run test 2>&1 | tail -20
```

Expected: `Failed to resolve import "./dashboardLayout"`.

- [ ] **Step 3: Write minimal implementation**

Создать `frontend/src/pages/admin/dashboardLayout.ts`:

```ts
import type { Layout } from 'react-grid-layout';

import type { DashboardWidget, WidgetPlacement } from '@/lib/api/dashboard';

/** Stored widgets → grid items. The item key is the widget id. */
export function toGridItems(widgets: DashboardWidget[]): Layout[] {
  return widgets.map((w) => ({ i: w.id, x: w.grid_x, y: w.grid_y, w: w.grid_w, h: w.grid_h }));
}

/**
 * Grid items → what PUT accepts. An item whose widget is no longer in the map
 * is dropped: the alternative is guessing a widget_type, and the server would
 * reject the guess with a 400 that the user cannot act on.
 */
export function toPlacements(items: Layout[], byId: Map<string, DashboardWidget>): WidgetPlacement[] {
  const out: WidgetPlacement[] = [];
  for (const item of items) {
    const widget = byId.get(item.i);
    if (!widget) continue;
    out.push({
      widget_type: widget.widget_type,
      grid_x: item.x,
      grid_y: item.y,
      grid_w: item.w,
      grid_h: item.h,
      options: widget.options,
    });
  }
  return out;
}
```

Создать `frontend/src/pages/admin/WidgetPicker.tsx`:

```tsx
import type { CatalogEntry } from '@/lib/api/dashboard';
import { Button } from '@/components/tabler/Button';
import { Card, CardBody, CardTitle } from '@/components/tabler/Card';

/** The "add a widget" list shown while edit mode is on. */
export function WidgetPicker({
  catalog,
  onAdd,
}: {
  catalog: CatalogEntry[];
  onAdd: (entry: CatalogEntry) => void;
}) {
  return (
    <Card className="mb-3">
      <CardBody>
        <CardTitle className="mb-3">Add a widget</CardTitle>
        <div className="row row-cards">
          {catalog.map((entry) => (
            <div key={entry.type} className="col-sm-6 col-lg-4">
              <div className="card card-sm">
                <CardBody>
                  <div className="fw-semibold">{entry.title}</div>
                  <div className="small text-secondary mb-2">{entry.description}</div>
                  <Button size="sm" variant="outline" onClick={() => onAdd(entry)}>
                    Add
                  </Button>
                </CardBody>
              </div>
            </div>
          ))}
        </div>
      </CardBody>
    </Card>
  );
}
```

Заменить `frontend/src/pages/admin/Dashboard.tsx`:

```tsx
import { useMemo, useState } from 'react';
import GridLayout, { type Layout } from 'react-grid-layout';
import 'react-grid-layout/css/styles.css';
import 'react-resizable/css/styles.css';

import { PageHeader } from '@/components/tabler/PageHeader';
import { Button } from '@/components/tabler/Button';
import { Card, CardBody, CardHeader, CardTitle } from '@/components/tabler/Card';
import { Alert } from '@/components/tabler/Alert';
import { Placeholder } from '@/components/tabler/Placeholder';
import { useDashboardLayout } from '@/hooks/useDashboardLayout';
import { WIDGET_COMPONENTS } from '@/widgets/registry';
import type { CatalogEntry, DashboardWidget, WidgetPlacement } from '@/lib/api/dashboard';

import { toGridItems, toPlacements } from './dashboardLayout';
import { WidgetPicker } from './WidgetPicker';

const GRID_COLUMNS = 12;
const ROW_HEIGHT = 72;

export function AdminDashboardPage() {
  const { layout, catalog, isLoading, error, save, saveError } = useDashboardLayout();
  // Edit mode is explicit: with dragging always live, reading the dashboard on a
  // touchpad quietly rewrites it.
  const [editing, setEditing] = useState(false);

  const widgets = layout ?? [];
  const byId = useMemo(() => new Map(widgets.map((w) => [w.id, w])), [widgets]);
  const items = useMemo(() => toGridItems(widgets), [widgets]);

  const onLayoutChange = (next: Layout[]) => {
    if (!editing) return;
    save(toPlacements(next, byId));
  };

  const addWidget = (entry: CatalogEntry) => {
    const placements: WidgetPlacement[] = [
      ...toPlacements(items, byId),
      {
        widget_type: entry.type,
        grid_x: 0,
        // Drop it below everything that is already placed.
        grid_y: widgets.reduce((max, w) => Math.max(max, w.grid_y + w.grid_h), 0),
        grid_w: entry.default_w,
        grid_h: entry.default_h,
      },
    ];
    save(placements);
  };

  const removeWidget = (id: string) => {
    save(toPlacements(items.filter((i) => i.i !== id), byId));
  };

  if (isLoading) return <div className="container-xl"><Placeholder /></div>;

  return (
    <>
      <PageHeader
        title="Dashboard"
        actions={
          <Button variant={editing ? 'primary' : 'outline'} onClick={() => setEditing((e) => !e)}>
            {editing ? 'Done' : 'Customise'}
          </Button>
        }
      />
      <div className="container-xl">
        {error && <Alert variant="danger">Failed to load the dashboard.</Alert>}
        {saveError && <Alert variant="danger">{saveError}</Alert>}
        {editing && catalog && <WidgetPicker catalog={catalog} onAdd={addWidget} />}

        {widgets.length === 0 ? (
          // Nothing is written to the database until the user places something:
          // an auto-seeded layout would be a choice made on their behalf.
          <div className="empty">
            <p className="empty-title">Your dashboard is empty</p>
            <p className="empty-subtitle text-secondary">
              Turn on Customise and add the widgets you want to see.
            </p>
          </div>
        ) : (
          <GridLayout
            className="layout"
            layout={items}
            cols={GRID_COLUMNS}
            rowHeight={ROW_HEIGHT}
            width={1200}
            isDraggable={editing}
            isResizable={editing}
            onLayoutChange={onLayoutChange}
          >
            {widgets.map((widget: DashboardWidget) => {
              const Component = WIDGET_COMPONENTS[widget.widget_type];
              return (
                <div key={widget.id}>
                  <Card className="h-100">
                    <CardHeader>
                      <CardTitle>{widget.widget_type}</CardTitle>
                      {editing && (
                        <Button
                          size="sm"
                          variant="ghost"
                          className="ms-auto"
                          onClick={() => removeWidget(widget.id)}
                        >
                          Remove
                        </Button>
                      )}
                    </CardHeader>
                    <CardBody>
                      {Component ? (
                        <Component options={widget.options} />
                      ) : (
                        <p className="text-secondary mb-0">Unknown widget: {widget.widget_type}</p>
                      )}
                    </CardBody>
                  </Card>
                </div>
              );
            })}
          </GridLayout>
        )}
      </div>
    </>
  );
}
```

Фиксированная `width={1200}` это временное значение: если на узком экране сетка обрезается, обернуть `GridLayout` в `WidthProvider` из `react-grid-layout` (`const ResponsiveGrid = WidthProvider(GridLayout)`), это один импорт и одна строка.

- [ ] **Step 4: Run test to verify it passes**

```sh
cd frontend && npm run test 2>&1 | tail -20 && npm run typecheck && npm run lint && npm run build
```

Expected: три кейса `dashboard layout mapping` зеленые, сборка проходит.

- [ ] **Step 5: Drive it in the browser**

```sh
make up
cd frontend && npm run dev
```

1. Зайти на `/admin` под админом: пустое состояние с предложением собрать дашборд.
2. Включить Customise, добавить все пять виджетов, увидеть данные в каждом.
3. Перетащить и растянуть карточку, выключить Customise, перезагрузить страницу: раскладка на месте.
4. В DevTools на вкладке Network убедиться, что за одно перетаскивание уходит один `PUT`, а не поток.
5. Зайти пользователем без `Administer`, но с `AuditRead`: в списке добавления только Audit.

- [ ] **Step 6: Commit**

```bash
git add frontend/src/pages/admin/Dashboard.tsx frontend/src/pages/admin/WidgetPicker.tsx \
        frontend/src/pages/admin/dashboardLayout.ts frontend/src/pages/admin/dashboardLayout.test.ts
git commit -m "feat(dashboard): customisable widget grid on /admin"
```

---

### Task 13: Удаление Tailwind и shadcn

Последняя задача: до нее Tailwind держал старую разметку читаемой, дальше он только мешает.

**Files:**
- Modify: `frontend/package.json`
- Delete: `frontend/tailwind.config.js`, `frontend/postcss.config.js`, `frontend/src/components/ui/` целиком
- Modify: `frontend/src/index.css`
- Modify: `frontend/src/lib/utils.ts`
- Modify: `frontend/src/routes/manifest.tsx`
- Modify: `frontend/public/theme.js`, `frontend/src/lib/theme.ts`

- [ ] **Step 1: Find what is still using the old stack**

```sh
cd frontend
grep -rn 'components/ui\|lucide-react\|tailwind\|twMerge\|class-variance-authority\|@radix-ui' src | sort
```

Каждое вхождение перевести на `@/components/tabler/*` и `@tabler/icons-react`. Список должен опустеть.

- [ ] **Step 2: Remove the packages and the config**

```sh
cd frontend
npm uninstall tailwindcss tailwind-merge tailwindcss-animate class-variance-authority \
              lucide-react @radix-ui/react-label @radix-ui/react-slot autoprefixer postcss
rm -f tailwind.config.js postcss.config.js
rm -rf src/components/ui
```

- [ ] **Step 3: Rewrite the leftovers**

`frontend/src/index.css` целиком (директивы Tailwind и токены shadcn уходят, Tabler приезжает из `main.tsx`):

```css
/* App-level overrides on top of @tabler/core. Tabler itself is imported from
   main.tsx after this file, so anything here that must win needs a selector,
   not just order. Theme colours come from Tabler's own CSS variables — this
   file only holds what the product adds. */

.page-body {
  /* Tabler's default top margin assumes a page-header on every page; ours
     sometimes starts straight with content. */
  margin-top: 1rem;
}

/* react-grid-layout's placeholder is a plain block; give it the card radius so
   a dragged widget lands on something that looks like the grid. */
.react-grid-item.react-grid-placeholder {
  background: var(--tblr-primary);
  border-radius: var(--tblr-border-radius);
  opacity: 0.2;
}
```

`frontend/src/lib/utils.ts`:

```ts
import { clsx, type ClassValue } from 'clsx';

/** Class joiner. twMerge is gone with Tailwind: Bootstrap classes do not need
 *  conflict resolution, the later class in the string simply wins. */
export function cn(...inputs: ClassValue[]) {
  return clsx(inputs);
}
```

`frontend/src/routes/manifest.tsx`: убрать импорт из `lucide-react`, убрать поле `navIcon` из интерфейса `RouteEntry` и из всех записей (боковая навигация его не рисует).

`frontend/public/theme.js` и `frontend/src/lib/theme.ts`: убрать переходную запись класса `dark` (в `theme.js` обе строки `classList.add('dark')`, в `theme.ts` строку `classList.toggle`) вместе с комментариями про переходный период. В `theme.test.ts` убрать оба ожидания про `classList`.

- [ ] **Step 4: Verify nothing is left**

```sh
cd frontend
grep -rn 'tailwind\|twMerge\|class-variance-authority\|lucide-react\|@radix-ui\|components/ui' src package.json ; echo "exit=$?"
npm run typecheck && npm run lint && npm run format:check && npm run test 2>&1 | tail -10
npm run build
```

Expected: `grep` не находит ничего (`exit=1`), все проверки зеленые, сборка проходит.

- [ ] **Step 5: Check the built bundle against the CSP**

```sh
cd frontend
grep -rn 'fonts.googleapis.com\|fonts.gstatic.com\|https://' dist/assets/*.css | head
```

Expected: ни одной внешней ссылки в собранном CSS. Внешний `url()` в стилях означал бы заблокированный ресурс под `font-src 'self'` и `img-src 'self' data:`.

- [ ] **Step 6: Full manual pass**

```sh
make up
```

Собрать фронт в контейнере и пройти по списку приемки из спека: визитка на `/`, `/blog.html`, `/blog/<slug>`, публикация поста, консоль браузера на `/admin` без ошибок CSP, раскладка переживает перезагрузку и не видна другому пользователю. Отдельно прогнать корневой `.browser-test.mjs`.

- [ ] **Step 7: Commit**

```bash
git add -A frontend
git commit -m "chore(admin): drop Tailwind, shadcn and lucide"
```
