# ТЗ: аудит и целевой контракт API блога (публичка + админ-контур)

Дата: 2026-07-25 · Статус: черновик на вычитке · Реализация — отдельной веткой по плану (writing-plans)

## 1. Контекст и цель

Фронтенд (статический BookCard + `blog.js` и React-админка) вырос быстрее, чем
публичный API: клиент выкачивает весь фид ради фильтров, дублирует бизнес-логику
(деривация topic), а SEO-ручки v1.7.0 написаны строковой конкатенацией в `.hpp`
и отдают `http://`-URL и сырые Postgres-даты.

Цель — привести контракт «фронт ↔ бэк» в порядок: убрать хаки, добавить
недостающие ручки, переписать SEO-поверхность начисто.

### Зафиксированные решения

| Вопрос | Решение |
|---|---|
| Скоуп | Публичная поверхность + админский контур блога (posts CRUD, uploads). Auth/jobs/audit — вне скоупа |
| Совместимость | **Ломаем v1 на месте** (единственный потребитель — свой фронт, деплой совместный). Никакого /api/v2 |
| Рендер | **API-first, без SSR-требования**: body отдаётся Markdown-ом, рендер клиентом (`marked.js`). Вопрос «HTML из БД» отложен осознанно |
| Форма контракта | **Hybrid**: ресурсные ручки + опциональный `?include=` для дорогих довесков (1 запрос = 1 страница, без женитьбы на UI) |
| `/blog-single.html` | **Удаляем полностью** (ручка, nginx, статик, фолбэк, спека). 404 по старым ссылкам — принятый риск |
| Глубина ТЗ | Требования + целевой контракт (ниже). Исполнитель реализует без свободы трактовок |

## 2. Карта текущего состояния и боли

### Публичный сайт (`frontend/public-site/js/blog.js`)

| Вызов | Страница | Боль |
|---|---|---|
| `GET /public/posts?limit=1000` | индекс `/blog.html` | весь фид (~729 карточек) целиком; тег-облако, фильтр, пагинация и **деривация topic** (`/^\d+-/` → "LeetCode") — на клиенте |
| `GET /public/posts/{slug}` | пост | body = Markdown (ок по решению), но ответ без соседей |
| `GET /public/posts?limit=1000` (повторно) | prev/next пейджер | вторая выкачка фида ради двух соседей |
| `GET /public/posts?limit=2` | «последние» на главной | ок |
| `POST /public/contact` | форма | ок |

### SEO-поверхность (`src/api/PublicPagesController.hpp`, v1.7.0)

- HTML/XML собирается `constexpr`-строками в заголовочном файле;
- origin берётся из `Host`/`X-Forwarded-Proto` → за TLS-терминирующим ingress
  выходит `http://` в sitemap `<loc>`, `og:url`, `og:image`, JSON-LD;
- даты в JSON-LD — сырой PG-timestamp (`2026-07-25 05:34:32.87643+00`),
  невалидно для schema.org (нужен ISO 8601);
- новые публичные роуты не были внесены в auth-allowlist → 401 в проде
  (устранено оверрайдом, закреплено в PR #5).

### Админка (React)

- список постов: только offset-пагинация — при 729 постах поиск поста руками;
- черновик нельзя посмотреть глазами читателя до публикации;
- uploads write-only: залитую картинку нельзя ни найти, ни удалить.

## 3. Целевой контракт

Все даты во всех ответах API — **ISO 8601 UTC** (`2026-07-25T05:34:32Z`).
Реализуется на уровне сериализации (`to_json` доменных моделей / репозитории),
не точечно в ручках.

### 3.1 Публичный контент

#### `GET /api/v1/public/posts` — единственный списочный ресурс

Параметры запроса:

| Параметр | Тип | Семантика |
|---|---|---|
| `page` | int, 1-based, default 1 | страница |
| `limit` | int, default 10, **max 50** (сервер клампит) | размер страницы |
| `topic` | string | фильтр, точное совпадение |
| `tag` | string | фильтр по тегу |
| `q` | string | поиск ILIKE по `title`+`summary` (не по body) |
| `include` | csv | `facets` — встроить фасеты |

Ответ:

```json
{
  "items": [
    { "slug": "...", "title": "...", "summary": "...", "topic": "...",
      "tags": ["..."], "read_mins": 4,
      "published_at": "2026-07-25T05:34:32Z", "updated_at": "..." }
  ],
  "page": 1, "limit": 10, "total": 727,
  "facets": {
    "topics": [ { "name": "SRE", "count": 12 } ],
    "tags":   [ { "name": "kubernetes", "count": 40 } ]
  }
}
```

Правила:
- карточки **никогда** не содержат `body`;
- сортировка фиксированная `published_at DESC` (без `sort`-параметра);
- `facets` присутствует только при `include=facets` и считается по **текущему
  фильтру** (топ-теги результата, как это сейчас делает JS);
- `topic` всегда заполнен сервером (см. миграцию 008);
- только `status='published'`.

Использование фронтом: индекс — `?page&topic&tag&include=facets`
(1 запрос на клик фильтра, мгновенность возвращает кэш TanStack/HTTP);
главная — `?limit=2`.

#### `GET /api/v1/public/posts/{slug}` — один пост

- `?include=adjacent` → добавить соседей:

```json
{
  "slug": "...", "title": "...", "summary": "...", "body": "<markdown>",
  "topic": "...", "tags": ["..."], "read_mins": 4,
  "published_at": "...", "updated_at": "...",
  "adjacent": { "prev": { "slug": "...", "title": "..." },
                "next": { "slug": "...", "title": "..." } }
}
```

- `prev`/`next` — соседние **published**-посты в порядке `published_at`;
  `null` на краях ленты;
- `?preview=<token>` — см. 3.3: валидный токен отдаёт draft, иначе draft → 404.

#### `POST /api/v1/public/contact` — без изменений.

### 3.2 SEO-поверхность (переписать начисто)

**Канонический origin из конфига.** Новый ключ `site.base_url` /
`SITE_BASE_URL` (прод: `https://tarassov.me`). В prod валидируется на старте
(непустой, https) — в общем блоке валидации конфига. Вся генерация абсолютных
URL (sitemap, og, JSON-LD) идёт от него; зависимость от `Host` /
`X-Forwarded-Proto` для генерации URL исключается.

**`GET /sitemap.xml`** — остаётся выделенной динамической ручкой:
- XML собирается нормальным писателем (escaping), не конкатенацией в `.hpp`;
- `<loc>` от `site.base_url`; `<lastmod>` в W3C-формате (`2026-07-25`);
- состав: `/`, `/blog.html`, все published-посты;
- `Cache-Control: max-age=3600`; Redis-кэш не вводим (YAGNI).

**`GET /blog/{slug}`** — SSR `<head>` остаётся (соц-скраперы не исполняют JS):
- **шаблоны в файлах** (Drogon CSP views или файловые шаблоны, грузятся на
  старте) — никакого HTML в `constexpr`-строках;
- JSON-LD: даты ISO 8601, все URL от `site.base_url`;
- should-have: **data island** — JSON поста в
  `<script type="application/json" id="post-data">`, `blog.js` гидрирует без
  повторного фетча;
- 404 по неизвестному slug — из того же шаблонного механизма;
- принимает `?preview=<token>` (см. 3.3).

**`GET /blog-single.html` — удалить полностью** (см. чек-лист в 5.5).

### 3.3 Админский контур

#### Поиск/фильтр: `GET /api/v1/posts` (форма ответа не меняется)

| Параметр | Семантика |
|---|---|
| `page`, `limit` | как сейчас |
| `q` | ILIKE по `title`+`slug`+`summary` |
| `status` | `draft` \| `published` |
| `topic`, `tag` | как в публичном списке |

#### Превью черновика

- `POST /api/v1/posts/{id}/preview-token` (админ-права как у posts CRUD) →

```json
{ "url": "/blog/<slug>?preview=<token>", "expires_at": "2026-07-25T13:00:00Z" }
```

- токен **stateless подписанный** (HMAC-инфраструктура `Tokens.hpp`),
  TTL 1 час, payload: post id + expiry; в БД не пишется, многоразовый в
  пределах TTL;
- проверяется в `GET /public/posts/{slug}` и `GET /blog/{slug}`;
- **инвариант:** списки, фасеты и sitemap не включают черновики никогда,
  с токеном или без.

#### Медиатека

- `GET /api/v1/admin/uploads?page&limit` →
  `{ "items": [ { "key", "url", "size_bytes", "content_type", "created_at" } ], "total": N }`;
- `DELETE /api/v1/admin/uploads/{key}` — `key` обязан быть одним URL-safe
  сегментом (загрузчик и так генерирует UUID-ключи; ключи со слэшами ручка
  отклоняет 400);
- бэкенд: `StorageBackend` получает `list()` и `remove()`
  (local — readdir; S3 — ListObjectsV2 / DeleteObject);
- трекинг «где используется картинка» сознательно **не** делаем (YAGNI).

### 3.4 Гигиена данных

- **Миграция 008:** `UPDATE posts SET topic='LeetCode' WHERE slug ~ '^\d+-'
  AND topic=''` — деривация из `blog.js` исполняется один раз в данных и
  удаляется из JS. Дальше topic — ответственность автора;
- теги: в БД остаются comma-joined TEXT; контракт API — всегда массив строк;
- `read_mins` в SQL — без изменений.

## 4. Явно вне скоупа

- SSR тела поста / HTML в БД (отложено отдельным решением);
- нормализация тегов в отдельную таблицу;
- related posts, RSS, полнотекстовый поиск по body;
- auth/account/jobs/audit-поверхность.

## 5. Критерии приёмки

1. **Спека — источник правды:** `docs/openapi.yaml` отражает каждый
   новый/изменённый/удалённый роут; `openapi-drift` + `routes-registered`
   зелёные; `frontend npm run gen:api` перегенерён и закоммичен.
2. **Фронт мигрирует в той же ветке** (ломаем на месте — без окна
   рассинхрона): `blog.js` — серверные фильтры + `include=facets/adjacent`,
   без `limit=1000` и `?slug=`-фолбэка; админка — контролы поиска/фильтра,
   кнопка «Preview», страница медиатеки.
3. **Auth-allowlist:** новые публичные пути внесены и в
   `helm/tarassov-me` `api.publicPaths`, и в `kDefaultPublicPathsCsv`
   (`src/utils/Strings.hpp`). `preview-token` — строго админский.
   Инвариант «preview не делает draft публичным» покрыт тестом.
4. **Интеграционные тесты:** фильтры/поиск/фасеты и границы пагинации
   (кламп limit=50); `adjacent` на первом/последнем посте; preview-токен
   (валидный / просроченный / чужой post id / draft без токена → 404);
   uploads list/delete; sitemap (только published, ISO lastmod,
   https-`<loc>`); `GET /blog-single.html` → 404.
5. **Чек-лист удаления `/blog-single.html`:** ручка в
   `PublicPagesController`, запись в `Endpoints.hpp`, секция в
   `docs/openapi.yaml`, nginx-location в `frontend/nginx.conf` **и**
   `helm/tarassov-me-frontend/templates/configmap.yaml` (синхрон),
   статик `frontend/public-site/blog-single.html`, `?slug=`-фолбэк в
   `blog.js`, упоминания в тестах.
6. **Конфиг:** `site.base_url` в `docs/CONFIG.md`, валидируется на старте
   в prod-режиме.
7. `CHANGELOG.md` (`## [Unreleased]`) обновлён.

## 6. Замечания для исполнителя

- Rate limiting: публичный список с `q` попадает под общий публичный
  лимитер — отдельного тира не требуется;
- деплой: новые публичные пути повторяют урок v1.7.0 — прод-релиз `api`
  использует live-оверрайд `api.publicPaths` (см. память проекта /
  PR #5), при выкатке убедиться, что оверрайд включает новые пути;
- PR #5 (`fix/public-pages-authpaths-and-proto`) частично перекрывается этим
  ТЗ: allowlist-часть остаётся нужной, nginx-proto-часть станет ненужной после
  перехода на `site.base_url` — согласовать при реализации.
