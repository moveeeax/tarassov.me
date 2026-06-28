# 0002 — nlohmann::json over jsoncpp

**Status:** Accepted

## Context

Drogon (ADR 0001) ships with jsoncpp for its `req->getJsonObject()` / response
helpers. We need to pick one JSON library and stick with it: mixing two means
every controller serializes from one type and deserializes through another,
which is the kind of friction that turns into bugs.

Tradeoffs:
- **jsoncpp**: comes with Drogon, no extra dep. C++03-era API, manual
  type juggling, weak compile-time guarantees.
- **nlohmann::json**: header-only, modern API (`json{{"key", value}}`),
  type-safe `.get<T>()`, idiomatic C++17/20, large community.

## Decision

Use `nlohmann::json` everywhere in app code. Treat Drogon's jsoncpp as an
internal implementation detail.

Concretely:
- Build response bodies with `nlohmann::json` and serialize via `.dump()`,
  passing the string to `HttpResponse::setBody()`.
- Parse incoming bodies with `json::parse(req->body())`. Do NOT call
  `req->getJsonObject()` — that returns jsoncpp.

## Consequences

- Two JSON libraries link into the binary. The footprint cost is real but
  small (both are header-mostly with few symbols).
- All examples and helper code (`make_json_response`, error builders) use
  `nlohmann::json`, so new contributors learn one API.
- Lost: Drogon's automatic JSON body deserialization on controller entry.
  We've decided the explicit `json::parse` call is worth it for the
  type-safe ergonomics afterward.

## Alternatives

If we ever wanted to drop the duplicate, `boost::json` is the next-most
modern option — but we'd still need a Drogon plugin to swap out jsoncpp
internally, which is more work than the current overhead is worth.
