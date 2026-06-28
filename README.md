# tarassov.me

Личный сайт Михаила Тарасова — визитка и блог на `tarassov.me`.

[![pipeline](https://gitlab.com/tarassov.me/site/badges/main/pipeline.svg)](https://gitlab.com/tarassov.me/site/-/pipelines)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Drogon](https://img.shields.io/badge/Drogon-HTTP-green.svg)
![PostgreSQL](https://img.shields.io/badge/PostgreSQL-15-336791.svg)
![Redis](https://img.shields.io/badge/Redis-7-DC382D.svg)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

## Что это

Публичная страница-визитка + блог с заметками, на собственном C++-бэкенде.
Контент блога редактируется через защищённую админку и хранится в Postgres;
картинки к постам — в S3/MinIO.

- **Публичный сайт** (`/`) — статическая визитка BookCard (`frontend/public-site/`):
  обо мне, опыт/стек, блог, контакт-форма.
- **Блог** — посты отдаются из API (`GET /api/v1/public/posts[/{slug}]`),
  рендер Markdown на клиенте.
- **Контакт-форма** — `POST /api/v1/public/contact` → письмо через SMTP (Mailer).
- **Админка** (`/admin/*`) — React/TS SPA: CRUD постов, загрузка изображений.

## Стек

| Слой | Технологии |
|---|---|
| Backend | C++20, Drogon (HTTP), libpqxx (PostgreSQL), redis-plus-plus |
| Frontend | React + TypeScript + Vite (админка), статический BookCard (визитка/блог) |
| Хранилище медиа | S3-совместимое (MinIO / R2 / S3), бэкенд local для dev |
| Инфраструктура | Docker Compose (dev), Helm (k8s), GitLab CI |

Один nginx-контейнер раздаёт BookCard на `/`, React-SPA как fallback
(`/admin`, `/login`, `/account/*`) и проксирует `/api/*` в бэкенд.

## Структура

```
src/            C++ backend (api/ domain/ repositories/ storage/ security/ email/ …)
frontend/       React-админка + public-site/ (BookCard) + nginx.conf + Dockerfile
migrations/     SQL-миграции (применяются на старте)
config/         config.json (dev) / config.production.json
helm/           чарты: tarassov-me (app) / -worker / -frontend / -env
docker/         docker-compose.yml + Dockerfile бэкенда
```

## Разработка

```sh
make up            # поднять стек (postgres + redis + app) в docker-compose
make test          # тесты (ctest в контейнере)
make build         # пересобрать образ приложения

cd frontend && npm install && npm run dev   # админка с hot-reload (проксирует /api)
make new-resource ENTITY=Foo                 # скаффолд CRUD-ресурса (домен+репо+контроллер+тесты)
```

Конфигурация — через `config/config.json` и переменные окружения
(каждый ключ читается как `${ENV:-default}`). Секреты — только через env /
external secrets, не в коде.

## Деплой

Образы публикуются в Docker Hub (`docker.io/resert/tarassov-me{,-worker,-frontend}`)
через GitLab CI. Прод — Helm-чарт `helm/tarassov-me` (+ `-worker` / `-frontend`).
Нужные CI-переменные и storage/SMTP-настройки описаны в `values.yaml` и
`docker/docker-compose.yml`.

## Лицензия

MIT — см. [LICENSE](LICENSE). Шаблон визитки BookCard — коммерческий
(куплен отдельно), не покрывается этой лицензией.
