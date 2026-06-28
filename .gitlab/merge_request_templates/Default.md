## What
<!-- One or two sentences. What does this MR do? -->

## Why
<!-- Linked ticket / motivation. Paste the issue link. -->

## How
<!-- Design notes for non-obvious choices; skip if it's a straightforward change. -->

## Tests
- [ ] Unit tests added/updated
- [ ] Integration tests cover the happy path
- [ ] Manual verification steps documented below (if any)

## Security
- [ ] No secrets introduced (grep for password/token/key)
- [ ] No new auth bypass paths (public_paths unchanged or justified)
- [ ] Input validated at the API boundary

## Breaking changes
<!-- List config keys / HTTP routes / response shapes that changed. Delete if none. -->

## Checklist
- [ ] `make test` passes locally
- [ ] `clang-format` clean
- [ ] Docs / OpenAPI updated (if public API changed)
- [ ] CHANGELOG / release notes updated (for user-facing changes)
