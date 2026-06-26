import { afterEach, describe, expect, it, vi } from 'vitest';

import { ApiClientError } from '@/lib/api/client';

/**
 * useMe is a React hook (useQuery), but its load-bearing behaviour lives in
 * the plain queryFn (fetchMe) and retry predicate (shouldRetryMe). The test
 * stack has no @testing-library/react, so we exercise those directly with a
 * mocked api client — same contract the route guard relies on:
 *   401  → resolve to null (logged out), never throw
 *   5xx  → throw so me.isError is true (don't pretend "logged out")
 */

const { get } = vi.hoisted(() => ({ get: vi.fn() }));
vi.mock('@/lib/api/client', async () => {
  const actual = await vi.importActual<typeof import('@/lib/api/client')>('@/lib/api/client');
  return { ...actual, api: { GET: get } };
});

// Imported after the mock is registered.
import { fetchMe, shouldRetryMe } from './useMe';

afterEach(() => {
  get.mockReset();
});

describe('fetchMe (useMe queryFn)', () => {
  it('returns the user on 200', async () => {
    const user = { id: 'u1', email: 'a@b.c' };
    get.mockResolvedValueOnce({ data: { user }, error: undefined });

    await expect(fetchMe()).resolves.toEqual(user);
    expect(get).toHaveBeenCalledWith('/api/v1/auth/me');
  });

  it('resolves to null on 401 without throwing (logged out)', async () => {
    get.mockResolvedValueOnce({
      data: undefined,
      error: new ApiClientError({ status: 401, message: 'missing_token' }),
    });

    await expect(fetchMe()).resolves.toBeNull();
  });

  it('throws on 5xx so the query goes to isError, not null', async () => {
    const err = new ApiClientError({ status: 503, message: 'service_unavailable' });
    get.mockResolvedValueOnce({ data: undefined, error: err });

    await expect(fetchMe()).rejects.toBe(err);
  });

  it('throws on a network error (status 0)', async () => {
    const err = new ApiClientError({ status: 0, message: 'Network error' });
    get.mockResolvedValueOnce({ data: undefined, error: err });

    await expect(fetchMe()).rejects.toBe(err);
  });

  it('throws when the response is ok but carries no data', async () => {
    get.mockResolvedValueOnce({ data: undefined, error: undefined });

    await expect(fetchMe()).rejects.toThrow(/failed to fetch \/me/);
  });
});

describe('shouldRetryMe (useMe retry policy)', () => {
  it('never retries a 401 (deliberate logged-out answer)', () => {
    const err = new ApiClientError({ status: 401, message: 'missing_token' });
    expect(shouldRetryMe(0, err)).toBe(false);
  });

  it('retries transient failures up to twice', () => {
    const err = new ApiClientError({ status: 503, message: 'boom' });
    expect(shouldRetryMe(0, err)).toBe(true);
    expect(shouldRetryMe(1, err)).toBe(true);
    expect(shouldRetryMe(2, err)).toBe(false);
  });

  it('retries non-ApiClientError throwables (e.g. the no-data Error)', () => {
    expect(shouldRetryMe(0, new Error('failed to fetch /me'))).toBe(true);
  });
});
