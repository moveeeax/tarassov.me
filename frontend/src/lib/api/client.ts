/**
 * Typed fetch wrapper over the OpenAPI `paths` tree.
 *
 * History: this used to be a hand-rolled vanilla wrapper with
 * caller-supplied generics (`api.GET<T>(path)`), so the URL, query
 * string, request body and response type were all unchecked. We now
 * parameterise the whole surface by the generated `paths` interface
 * (docs/openapi.yaml → schema.gen.ts) so a wrong path, a typo in
 * `params.query`, a malformed body, or the wrong response shape is a
 * *compile* error. The runtime engine (refresh-on-401 + the
 * { data, error } envelope) is unchanged — only the types got stricter.
 *
 * Two ways in:
 *   - `api.GET / POST / PATCH / DELETE`  → typed { data?, error? } pair.
 *   - `api.getJson / postJson / …`       → unwrap-or-throw one-liners.
 * Both accept a typed path. A string fallback overload remains for
 * escape hatches (and for tests that probe non-spec paths); it returns
 * `unknown` so the typed overloads stay the ergonomic default.
 *
 * `credentials: 'include'` ships the HttpOnly auth cookies set by
 * /api/auth/login. Same-origin in dev (Vite proxy) and prod (nginx
 * proxy_pass) so SameSite=Lax keeps them on without CORS flags.
 *
 * Session refresh: the access cookie lives ~15 minutes; the refresh
 * cookie ~7 days. On a 401 from a refreshable path we POST
 * /api/auth/refresh once (deduplicated across concurrent callers) and
 * retry the original request — so an idle tab resumes seamlessly
 * instead of bouncing the user to /login every 15 minutes.
 *
 * The refresh is suppressed only for the four credential endpoints
 * (login / register / refresh / logout) where a 401 is a real answer,
 * not an expired access cookie. /api/auth/me is explicitly NOT in that
 * set: an idle tab's access cookie expires, /me 401s, and we must
 * refresh+replay it — otherwise useMe() resolves to null and the guard
 * bounces a still-valid session to /login.
 */

import type { paths } from './schema.gen';

export interface ApiFieldError {
  field?: string;
  code?: string;
  message?: string;
}

/** Backend error envelope: { error, status, message?, errors? }. */
export interface ApiError {
  status: number;
  error?: string;
  message?: string;
  errors?: ApiFieldError[];
  [key: string]: unknown;
}

/**
 * Error class thrown by the *Json helpers and returned in the
 * { error } slot of the api.* methods. Carries the backend envelope
 * as first-class fields:
 *   - status: HTTP status (0 = the request never reached the server)
 *   - code:   machine-readable backend code (the envelope's `error`)
 *   - fields: field-level validation details (the envelope's `errors`)
 *
 * It also exposes `error` / `errors` aliases so it structurally
 * satisfies the ApiError envelope shape and keeps apiErrorMessage and
 * existing `error.status` call sites working unchanged.
 */
export class ApiClientError extends Error {
  readonly status: number;
  readonly code?: string;
  readonly fields?: ApiFieldError[];
  /** Raw parsed response body, when there was one. */
  readonly body?: unknown;

  constructor(opts: {
    status: number;
    message: string;
    code?: string;
    fields?: ApiFieldError[];
    body?: unknown;
  }) {
    super(opts.message);
    this.name = 'ApiClientError';
    this.status = opts.status;
    this.code = opts.code;
    this.fields = opts.fields;
    this.body = opts.body;
  }

  /** Envelope alias: the backend's `error` code. */
  get error(): string | undefined {
    return this.code;
  }

  /** Envelope alias: the backend's `errors` array. */
  get errors(): ApiFieldError[] | undefined {
    return this.fields;
  }
}

export interface ApiResult<T> {
  data?: T;
  error?: ApiClientError;
  /** Raw Response — absent when the request never reached the server. */
  response?: Response;
}

interface RequestOptions {
  body?: unknown;
  /** Typed query string — serialised onto the URL. */
  query?: Record<string, unknown>;
  signal?: AbortSignal;
  headers?: Record<string, string>;
}

// ── Type plumbing: project `paths` into per-method path keys + ─────────────
//    request body / response / query types. Mirrors what openapi-fetch does
//    internally, kept local so we own the { data, error } + refresh engine.

type HttpMethod = 'get' | 'post' | 'patch' | 'delete' | 'put';

/** Path keys that declare the given method as a real operation (not `never`). */
type PathsWith<M extends HttpMethod> = {
  [P in keyof paths]: paths[P] extends Record<M, infer O>
    ? O extends Record<string, unknown>
      ? P
      : never
    : never;
}[keyof paths];

/** The operation object for (path, method). */
type Op<P extends keyof paths, M extends HttpMethod> =
  M extends keyof paths[P] ? paths[P][M] : never;

/** Pluck the application/json content from a `{ content: {...} }` wrapper. */
type JsonContent<T> = T extends { content: { 'application/json': infer J } } ? J : never;

/** 2xx JSON response body for (path, method) — 200 preferred, then 201. */
type ResponseOf<P extends keyof paths, M extends HttpMethod> =
  Op<P, M> extends { responses: infer R }
    ? 200 extends keyof R
      ? JsonContent<R[200]>
      : 201 extends keyof R
        ? JsonContent<R[201]>
        : unknown
    : unknown;

/** JSON request body for (path, method); `never` when the op takes no body. */
type BodyOf<P extends keyof paths, M extends HttpMethod> =
  Op<P, M> extends { requestBody: infer RB }
    ? JsonContent<RB>
    : Op<P, M> extends { requestBody?: infer RB }
      ? RB extends undefined
        ? never
        : JsonContent<NonNullable<RB>>
      : never;

/** Typed query object for (path, method); empty when the op declares none. */
type QueryOf<P extends keyof paths, M extends HttpMethod> =
  Op<P, M> extends { parameters: { query?: infer Q } }
    ? [Q] extends [undefined]
      ? Record<string, never>
      : NonNullable<Q>
    : Record<string, never>;

/**
 * Per-method typed options. A single optional parameter (so `P` is always
 * inferred from the path argument first — a conditional *rest tuple* here
 * defeats inference and makes TS silently fall through to the string
 * fallback overload). `body` / `query` carry the precise per-operation
 * types; both stay optional so the overload still matches a bare call,
 * while a wrong body/query shape is still a compile error.
 */
type TypedOptions<P extends keyof paths, M extends HttpMethod> = {
  signal?: AbortSignal;
  headers?: Record<string, string>;
} & ([BodyOf<P, M>] extends [never] ? { body?: undefined } : { body?: BodyOf<P, M> }) &
  ([keyof QueryOf<P, M>] extends [never] ? { query?: undefined } : { query?: QueryOf<P, M> });

/**
 * Single in-flight refresh shared by all concurrent 401 handlers —
 * ten parallel queries hitting an expired access cookie trigger one
 * /api/auth/refresh, not ten.
 */
let refreshInFlight: Promise<boolean> | null = null;

/**
 * Credential endpoints whose 401 is a genuine answer, not an expired
 * access cookie — refreshing + replaying them makes no sense (and would
 * loop on /refresh itself). Everything else, including /api/auth/me, is
 * refreshable. Matched as path prefixes so query strings don't matter.
 */
const NO_REFRESH_PREFIXES = [
  '/api/v1/auth/login',
  '/api/v1/auth/register',
  '/api/v1/auth/refresh',
  '/api/v1/auth/logout',
];

function isRefreshable(path: string): boolean {
  return !NO_REFRESH_PREFIXES.some((p) => path === p || path.startsWith(p + '?'));
}

function tryRefresh(): Promise<boolean> {
  refreshInFlight ??= fetch('/api/v1/auth/refresh', {
    method: 'POST',
    credentials: 'include',
    headers: csrfHeader('POST'),
  })
    .then((r) => r.ok)
    .catch(() => false)
    .finally(() => {
      refreshInFlight = null;
    });
  return refreshInFlight;
}

/** Append a typed query object to the path, dropping undefined/null values. */
function withQuery(path: string, query?: Record<string, unknown>): string {
  if (!query) return path;
  const usp = new URLSearchParams();
  for (const [k, v] of Object.entries(query)) {
    if (v === undefined || v === null) continue;
    usp.append(k, String(v));
  }
  const qs = usp.toString();
  if (!qs) return path;
  return path + (path.includes('?') ? '&' : '?') + qs;
}

// ── Double-submit CSRF ──────────────────────────────────────────────────────
// When the backend has security.csrf.enabled, it sets a NON-HttpOnly cookie
// (default `csrf-token`) on login/refresh that we echo back in a header on
// state-changing requests. The backend rejects a cookie-authenticated mutation
// whose header doesn't match the cookie. When CSRF is disabled the cookie is
// absent and csrfHeader() is a no-op, so this is safe regardless of config.
const CSRF_COOKIE = 'csrf-token';
const CSRF_HEADER = 'X-CSRF-Token';
const UNSAFE_METHODS = new Set(['POST', 'PUT', 'PATCH', 'DELETE']);

function readCookie(name: string): string | undefined {
  if (typeof document === 'undefined') return undefined;
  for (const part of document.cookie.split('; ')) {
    const eq = part.indexOf('=');
    if (eq > 0 && part.slice(0, eq) === name) return decodeURIComponent(part.slice(eq + 1));
  }
  return undefined;
}

function csrfHeader(method: string): Record<string, string> {
  if (!UNSAFE_METHODS.has(method.toUpperCase())) return {};
  const token = readCookie(CSRF_COOKIE);
  return token ? { [CSRF_HEADER]: token } : {};
}

async function rawRequest(method: string, path: string, opts: RequestOptions): Promise<Response> {
  const init: RequestInit = {
    method,
    credentials: 'include',
    headers: {
      Accept: 'application/json',
      ...(opts.body !== undefined ? { 'Content-Type': 'application/json' } : {}),
      ...csrfHeader(method),
      ...opts.headers,
    },
    body: opts.body !== undefined ? JSON.stringify(opts.body) : undefined,
    signal: opts.signal,
  };
  return fetch(path, init);
}

function errorFromBody(status: number, parsed: unknown, fallbackMessage: string): ApiClientError {
  if (parsed && typeof parsed === 'object') {
    const env = parsed as Partial<ApiError>;
    const code = typeof env.error === 'string' ? env.error : undefined;
    const fields = Array.isArray(env.errors) ? env.errors : undefined;
    const message =
      typeof env.message === 'string' && env.message ? env.message : (code ?? fallbackMessage);
    return new ApiClientError({ status, message, code, fields, body: parsed });
  }
  return new ApiClientError({ status, message: fallbackMessage, body: parsed });
}

async function request<T>(
  method: string,
  rawPath: string,
  opts: RequestOptions = {},
): Promise<ApiResult<T>> {
  const path = withQuery(rawPath, opts.query);
  let response: Response;
  try {
    response = await rawRequest(method, path, opts);

    // Expired access cookie? Refresh once and replay. Only the credential
    // endpoints (login/register/refresh/logout) are excluded: their 401 is
    // a real answer. /api/auth/me IS refreshable so an idle tab resumes.
    // One replay max — a second 401 is returned as-is, never a loop.
    if (response.status === 401 && isRefreshable(path)) {
      if (await tryRefresh()) {
        response = await rawRequest(method, path, opts);
      }
    }
  } catch (cause) {
    // Deliberate cancellation isn't a network failure — let TanStack
    // Query (which owns the AbortSignal) see the original rejection.
    if (cause instanceof DOMException && cause.name === 'AbortError') throw cause;
    // fetch() rejected: DNS failure, connection refused, offline…
    // Surface it through the same { error } channel instead of an
    // unhandled rejection in every call site.
    return { error: new ApiClientError({ status: 0, message: 'Network error' }) };
  }

  const text = await response.text();
  let parsed: unknown = undefined;
  if (text) {
    try {
      parsed = JSON.parse(text);
    } catch {
      // non-JSON — leave undefined
    }
  }

  if (!response.ok) {
    const fallback =
      text && parsed === undefined ? text : response.statusText || `HTTP ${response.status}`;
    return { error: errorFromBody(response.status, parsed, fallback), response };
  }
  return { data: parsed as T, response };
}

/**
 * Unwrapping variant: returns the parsed body or throws ApiClientError.
 * Collapses the `const { data, error } = …; if (error || !data) throw …`
 * boilerplate every TanStack queryFn/mutationFn was repeating.
 */
async function fetchJson<T>(method: string, path: string, opts: RequestOptions = {}): Promise<T> {
  const { data, error } = await request<T>(method, path, opts);
  if (error) throw error;
  if (data === undefined)
    throw new ApiClientError({ status: 0, message: `${method} ${path} returned an empty body` });
  return data;
}

// ── Public surface ─────────────────────────────────────────────────────────
//
// Each verb has a typed overload (path keyed into `paths`, body/query/response
// all inferred) plus a permissive string fallback (returns unknown). The typed
// overload is selected whenever the path is a known literal.

interface ApiSurface {
  GET<P extends PathsWith<'get'>>(
    path: P,
    opts?: TypedOptions<P, 'get'>,
  ): Promise<ApiResult<ResponseOf<P, 'get'>>>;
  GET(path: string, opts?: RequestOptions): Promise<ApiResult<unknown>>;

  POST<P extends PathsWith<'post'>>(
    path: P,
    opts?: TypedOptions<P, 'post'>,
  ): Promise<ApiResult<ResponseOf<P, 'post'>>>;
  POST(path: string, opts?: RequestOptions): Promise<ApiResult<unknown>>;

  PATCH<P extends PathsWith<'patch'>>(
    path: P,
    opts?: TypedOptions<P, 'patch'>,
  ): Promise<ApiResult<ResponseOf<P, 'patch'>>>;
  PATCH(path: string, opts?: RequestOptions): Promise<ApiResult<unknown>>;

  DELETE<P extends PathsWith<'delete'>>(
    path: P,
    opts?: TypedOptions<P, 'delete'>,
  ): Promise<ApiResult<ResponseOf<P, 'delete'>>>;
  DELETE(path: string, opts?: RequestOptions): Promise<ApiResult<unknown>>;

  getJson<P extends PathsWith<'get'>>(
    path: P,
    opts?: TypedOptions<P, 'get'>,
  ): Promise<ResponseOf<P, 'get'>>;
  getJson<T = unknown>(path: string, opts?: RequestOptions): Promise<T>;

  postJson<P extends PathsWith<'post'>>(
    path: P,
    opts?: TypedOptions<P, 'post'>,
  ): Promise<ResponseOf<P, 'post'>>;
  postJson<T = unknown>(path: string, opts?: RequestOptions): Promise<T>;

  patchJson<P extends PathsWith<'patch'>>(
    path: P,
    opts?: TypedOptions<P, 'patch'>,
  ): Promise<ResponseOf<P, 'patch'>>;
  patchJson<T = unknown>(path: string, opts?: RequestOptions): Promise<T>;

  deleteJson<P extends PathsWith<'delete'>>(
    path: P,
    opts?: TypedOptions<P, 'delete'>,
  ): Promise<ResponseOf<P, 'delete'>>;
  deleteJson<T = unknown>(path: string, opts?: RequestOptions): Promise<T>;
}

export const api: ApiSurface = {
  GET: (path: string, opts?: RequestOptions) => request('GET', path, opts),
  POST: (path: string, opts?: RequestOptions) => request('POST', path, opts),
  PATCH: (path: string, opts?: RequestOptions) => request('PATCH', path, opts),
  DELETE: (path: string, opts?: RequestOptions) => request('DELETE', path, opts),
  getJson: (path: string, opts?: RequestOptions) => fetchJson('GET', path, opts),
  postJson: (path: string, opts?: RequestOptions) => fetchJson('POST', path, opts),
  patchJson: (path: string, opts?: RequestOptions) => fetchJson('PATCH', path, opts),
  deleteJson: (path: string, opts?: RequestOptions) => fetchJson('DELETE', path, opts),
} as ApiSurface;

export function apiErrorMessage(error: unknown, fallback = 'Something went wrong'): string {
  if (typeof error === 'object' && error !== null) {
    const e = error as Partial<ApiError>;
    // Surface field-level validation details when the backend sent them —
    // "email: bad_format" beats a generic "validation_failed".
    // ApiClientError satisfies this via its `errors`/`error` aliases.
    if (Array.isArray(e.errors) && e.errors.length > 0) {
      const fields = e.errors
        .map((f) => [f.field, f.message ?? f.code].filter(Boolean).join(': '))
        .filter(Boolean)
        .join('; ');
      if (fields) return fields;
    }
    return e.message || e.error || fallback;
  }
  return fallback;
}
