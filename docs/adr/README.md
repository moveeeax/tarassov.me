# Architecture Decision Records

ADRs capture *why* we made a structural choice — the constraint, the
alternatives that were considered, and the tradeoffs we accepted. Read
the code for *what*; read these for *why*.

Every ADR is immutable once accepted. If a decision is reversed, write a new
record that supersedes the old one (and link both ways).

## Format

- `NNNN-kebab-title.md` — zero-padded sequence number, lowercase title
- Sections: `Status`, `Context`, `Decision`, `Consequences`, `Alternatives`
- New ADRs start with `Status: Proposed`, flip to `Accepted` on merge

## Index

- [0001 — Drogon as the HTTP framework](0001-drogon-http-framework.md)
- [0002 — nlohmann::json over jsoncpp](0002-nlohmann-json.md)
- [0003 — Header-only module layout](0003-header-only-modules.md)
- [0004 — Global singletons for subsystems](0004-global-singletons.md)
