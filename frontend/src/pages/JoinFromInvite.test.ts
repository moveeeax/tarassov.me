import { afterEach, describe, expect, it, vi } from 'vitest';

import { ApiClientError } from '@/lib/api/client';

/**
 * JoinFromInvitePage renders a form (no @testing-library in the stack), so we
 * test the load-bearing submit logic it delegates to: submitJoinFromInvite.
 *   - error response  → ok:false with a user-facing message (the page shows
 *                       it in a destructive Alert)
 *   - success         → ok:true (the page flips to the "log in now" success
 *                       state with a link to /login)
 */

const { post } = vi.hoisted(() => ({ post: vi.fn() }));
vi.mock('@/lib/api/client', async () => {
  const actual = await vi.importActual<typeof import('@/lib/api/client')>('@/lib/api/client');
  return { ...actual, api: { POST: post } };
});

import { submitJoinFromInvite } from './JoinFromInvite';

afterEach(() => {
  post.mockReset();
});

describe('submitJoinFromInvite', () => {
  it('posts the new password to the token-scoped endpoint and returns ok on success', async () => {
    post.mockResolvedValueOnce({ data: { ok: true }, error: undefined });

    await expect(submitJoinFromInvite('tok123', 'hunter2pw')).resolves.toEqual({ ok: true });
    expect(post).toHaveBeenCalledWith('/api/v1/account/join-from-invite/tok123', {
      body: { new_password: 'hunter2pw' },
    });
  });

  it('url-encodes the token', async () => {
    post.mockResolvedValueOnce({ data: { ok: true }, error: undefined });

    await submitJoinFromInvite('a/b c', 'hunter2pw');
    expect(post).toHaveBeenCalledWith('/api/v1/account/join-from-invite/a%2Fb%20c', {
      body: { new_password: 'hunter2pw' },
    });
  });

  it('returns ok:false with the backend message on error', async () => {
    post.mockResolvedValueOnce({
      data: undefined,
      error: new ApiClientError({ status: 400, message: 'token expired' }),
    });

    await expect(submitJoinFromInvite('tok', 'hunter2pw')).resolves.toEqual({
      ok: false,
      error: 'token expired',
    });
  });

  it('falls back to a friendly message when the error has no detail', async () => {
    post.mockResolvedValueOnce({
      data: undefined,
      error: new ApiClientError({ status: 410, message: '' }),
    });

    const result = await submitJoinFromInvite('tok', 'hunter2pw');
    expect(result).toEqual({
      ok: false,
      error: 'This invitation link is invalid or has expired.',
    });
  });
});
