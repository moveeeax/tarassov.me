import { useQuery } from '@tanstack/react-query';

import { api, ApiClientError } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { MeResponse } from '@/lib/api/types';

/**
 * Query fn for useMe, exported for unit tests (no @testing-library).
 * 401 → null (logged out); anything else throws so the guard can retry
 * instead of bouncing a still-valid session to /login.
 */
export async function fetchMe(): Promise<MeResponse['user'] | null> {
  const { data, error } = await api.GET('/api/v1/auth/me');
  if (error) {
    if (error.status === 401) return null; // no session
    throw error; // network / 5xx — surface as a real error
  }
  if (!data) throw new Error('failed to fetch /me');
  return data.user;
}

/**
 * Retry predicate for useMe: never retry a deliberate "logged out" (401);
 * retry transient failures up to twice. Exported for the same reason as
 * fetchMe — it carries the "don't bounce on a blip" policy.
 */
export function shouldRetryMe(failureCount: number, error: unknown): boolean {
  if (error instanceof ApiClientError && error.status === 401) return false;
  return failureCount < 2;
}

export function useMe() {
  return useQuery({
    queryKey: qk.me(),
    queryFn: fetchMe,
    // Don't retry a deliberate "logged out" answer; do retry transient
    // failures (default exponential backoff applies to thrown errors).
    retry: shouldRetryMe,
  });
}
