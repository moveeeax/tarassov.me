# Contributing

By participating you agree to our [Code of Conduct](CODE_OF_CONDUCT.md). For
security issues, follow [SECURITY.md](SECURITY.md) — don't open a public issue.

## One-time setup

```bash
pipx install pre-commit   # or: pip install --user pre-commit
pre-commit install
```

This wires `.pre-commit-config.yaml` into a git hook. Each commit runs:

- `clang-format` on touched `src/`, `tests/` C/C++ files (`-Werror`-equivalent).
- `shellcheck` on `scripts/*.sh` and other shell-shebang files.
- Trailing-whitespace / final-newline / merge-conflict-marker / mixed-line-ending guards.
- YAML / JSON parse validation.
- A large-file guard (rejects blobs > 512 KiB).
- `gitleaks` — secret-scanning over the staged diff.
- `scripts/check-openapi-drift.sh` — only when `src/api/Endpoints.hpp` or
  `docs/openapi.yaml` is touched. Verifies `(method, path)` tuples match
  on both sides; catches verb-only changes the path-only diff missed.
- `scripts/lint-openapi.sh` (Spectral) on `docs/openapi.yaml` — best-effort,
  skips if no `npx`/`docker`/`spectral` binary is around.

To force-check everything once (not just touched files):

```bash
pre-commit run --all-files
```

## Development workflow

1. Branch from `master`.
2. Make focused, reviewable commits — ideally one per logical change.
3. Run `make test` locally before pushing (or `make test-quick` for the
   fast TDD loop against an already-built image).
4. Open an MR; CI will run build, tests, sanitizers, and linters.
5. Wait for required reviewers (see `CODEOWNERS`).

## Commit messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
type(scope): short summary

Longer body if needed — explain *why*, not *what*.

Closes #123
```

Types: `feat`, `fix`, `refactor`, `perf`, `docs`, `test`, `build`, `ci`,
`chore`. Scope is optional but recommended (e.g. `feat(auth):`, `fix(jobs):`).

## Code style

- C++20. Header-only modules under `src/`, single `main.cpp` per binary.
- `clang-format` (`.clang-format` in repo) is enforced by CI — run locally
  before pushing.
- `clang-tidy` config in `.clang-tidy` — new code should not regress the lint
  baseline (see CI `lint:clang-tidy`).
- Prefer `std::` containers and smart pointers; raw `new`/`delete` is a red
  flag that needs a justification comment.
- Thread-safety: document which entities are thread-safe and which aren't.

## Tests

- Every new module in `src/` gets a `tests/unit/test_<module>.cpp`.
- Integration tests (need Postgres/Redis) go in `tests/integration/` and
  skip themselves when `full_init` is false.
- Failing a new test is a blocker; flaky tests must be either fixed, marked
  `DISABLED_`, or filed as an issue.

## Security

- Never commit secrets. `config/*.json` must use `${VAR}` placeholders.
- Anything touching `src/security/` requires review by the security team
  (see `CODEOWNERS`).
- Report vulnerabilities privately per `SECURITY.md`.

## Pull requests

The PR template covers:

- **What** — one-sentence description.
- **Why** — motivation / linked ticket.
- **How** — design notes only if non-obvious.
- **Tests** — which tests cover the change.
- **Breaking changes** — list them explicitly (config keys, API routes,
  response shapes).

## Two CI pipelines

The repo ships both `.github/workflows/` (GitHub Actions) and `.gitlab-ci.yml`
(GitLab) so it works on either host. They are kept close but are NOT identical
— pick the one your host uses; the other is a maintained reference. Current
intentional differences:

- **GitLab** additionally runs `cppcheck` and a Spectral OpenAPI lint.
- Both now run Trivy image scanning, gitleaks, shellcheck, and the
  openapi-drift + test-bucket checks.
- Both emit SBOM + provenance on release images.

When you change a CI gate, change it in **both** files (or note here why it's
intentionally one-sided).

## Release

Semver, tagged on `main`. A tag that matches `v*.*.*` triggers
`.github/workflows/release.yml`, which builds multi-arch images for both
the app and worker targets, pushes them to `ghcr.io/<owner>/<repo>` (and
`-worker`), and opens a draft GitHub Release seeded from auto-generated
commit notes. Update `CHANGELOG.md` under `## [Unreleased]` before tagging.
