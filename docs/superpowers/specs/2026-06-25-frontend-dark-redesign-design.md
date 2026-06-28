# Frontend redesign — "refined dev-tool, dark-first, indigo"

Date: 2026-06-25 · Status: approved (direction + accent)

## Problem

1. **Layout still shifts on notifications.** Server/form errors (invalid login,
   etc.) render an in-flow `FormAlert`; even with its grid-rows animation the
   alert occupies flow space and pushes the fields — the shift was smoothed, not
   removed. The user wants zero movement.
2. **The UI reads as a templated default** (stock shadcn/Tailwind) — no point of
   view.

## Direction (approved)

Refined developer-tool aesthetic (Linear / Vercel): **dark by default**, tight
spacing, **thin borders over drop-shadows**, monospace accents for technical
content, a single restrained **indigo** accent. Light mode stays available via
the existing toggle.

## Where the theme lives (grounding)

- Tokens: CSS variables in `src/index.css` (`:root` + `.dark`), consumed via
  `tailwind.config` (`darkMode: ['class']`, colors are `hsl(var(--x))`).
- Dark is toggled by the `dark` class on `document.documentElement`
  (`Nav.tsx`), persisted in `localStorage.theme`.

## Design

### 1. Tokens (the core of the redesign)

Rework the `.dark` palette (and tune `:root` light to match the new neutrals)
in `src/index.css`, keeping the existing variable names so every component
inherits automatically:

- **Neutrals → zinc.** base `--background` ≈ zinc-950, `--card` ≈ zinc-900,
  `--muted` ≈ zinc-800, `--border`/`--input` ≈ zinc-800, `--foreground` ≈
  zinc-100, `--muted-foreground` ≈ zinc-400.
- **Accent → indigo.** `--primary` ≈ indigo-500 (dark: a touch lighter for
  contrast), `--ring` ≈ indigo. Links/hover use indigo-400 on dark.
- **Semantic:** keep destructive (red), add/keep success (emerald) + warning
  (amber) tokens with dark-correct values.
- **Radius:** `--radius` 0.5rem → **0.375rem** (tighter).
- **Density:** introduce a convention — page wrapper `py-8` (was `py-12`),
  tighter card/table padding. Applied at component/page level, not a token.
- **Default theme = dark:** set the `dark` class on first load unless
  `localStorage.theme === 'light'` (small inline/init step), so the app opens
  dark.
- **Type:** keep the sans for UI; use `font-mono` deliberately for ids/codes/
  actions (already partial in DataTable); calm the heading scale.

### 2. Notifications → toast (out of flow) — the real shift fix

Add a tiny, dependency-free toast system:

- `src/components/ui/toaster.tsx`: a `ToastProvider` (mounted once in `Layout`)
  + `useToast()` returning `{ error, success, info, show }`. Toasts render
  `fixed bottom-right`, stacked, auto-dismiss (~5 s), manually dismissible,
  variant-styled (reuses Alert variants), `role="status"` + `aria-live`.
- Because they're `position: fixed`, **they never affect document layout** →
  zero CLS.
- Wire forms: on server/submit error → `toast.error(apiErrorMessage(e))`; on a
  successful action that previously showed an inline banner → `toast.success`.
- **Field-level validation stays inline** under each field (`FormField`) — it's
  expected and small. Only the form-level/server banner moves to a toast.
- **Remove `FormAlert`** (the previous half-fix) and its usages.

### 3. Component refresh (inherit tokens + targeted polish)

- **Button** (`ui/button-variants.ts`): indigo primary, tighter radius, clear
  `focus-visible` ring; refine ghost/destructive on dark.
- **Card** (`ui/card.tsx`): zinc-900 surface, thin border, minimal shadow,
  tighter default padding.
- **Input** (`ui/input.tsx`): dark surface, subtle border, indigo focus ring.
- **Nav**: restyle the existing active indicator to an indigo accent; dark
  surface + bottom border.
- **DataTable**: denser rows, thin zinc borders, subtle hover, `font-mono` for
  id-like cells.
- **Job status badges** (`pages/admin/Jobs.tsx`): refined dark palette.
- **Alert** stays (used by toasts + field area) — already has aria-live.

### 4. Scope / non-goals

- In scope: `index.css` tokens + dark default, toast system, the component
  refreshes above, page-level spacing/heading tidy, replacing FormAlert with
  toasts across the forms.
- Out of scope: no new pages/features, no router/data changes, no new deps, no
  backend/helm changes.

## Acceptance criteria

- Triggering a failed login (and any server error / success) produces a toast;
  **the form does not move** (visually verify; the alert is no longer in flow).
- App opens in dark by default; the light toggle still works and looks correct.
- Tokens are consistent across all pages (no light-mode-only or ad-hoc colors
  left, e.g. job badges).
- `npm run typecheck && npm run lint && npm test && npm run build` all green.

## Rollout

Implement on a branch, verify, ship as **v1.3.3** (frontend image), redeploy
the prod frontend (api/worker unchanged).
