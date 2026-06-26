import { describe, expect, it } from 'vitest';

import { guardDecision, type GuardSessionState } from './ProtectedRoute';

/**
 * ProtectedRoute renders JSX (no @testing-library in the stack), but its
 * routing policy is the pure guardDecision() it delegates to. These tests
 * pin the round-2 invariant: a 5xx/network error shows the error/retry
 * state and NEVER redirects to /login, while a real "no session" (data ===
 * null, from a 401) does redirect.
 */

const always = () => true;
const never = () => false;

function state(p: Partial<GuardSessionState>): GuardSessionState {
  return { isPending: false, isError: false, data: undefined, ...p };
}

describe('guardDecision', () => {
  it('loading while the query is pending', () => {
    expect(guardDecision(state({ isPending: true }), {}, always)).toEqual({ kind: 'loading' });
  });

  it('isError → error state, NOT a /login redirect', () => {
    const d = guardDecision(state({ isError: true }), {}, always);
    expect(d).toEqual({ kind: 'error' });
    // Explicitly assert we never bounce on a transient failure.
    expect(d.kind).not.toBe('redirect');
  });

  it('data === null (401) → redirect to /login', () => {
    expect(guardDecision(state({ data: null }), {}, always)).toEqual({
      kind: 'redirect',
      to: '/login',
    });
  });

  it('authenticated user → allow', () => {
    expect(guardDecision(state({ data: { confirmed: true } }), {}, always)).toEqual({
      kind: 'allow',
    });
  });

  it('requireConfirmed + unconfirmed → redirect to /unconfirmed', () => {
    expect(
      guardDecision(state({ data: { confirmed: false } }), { requireConfirmed: true }, always),
    ).toEqual({ kind: 'redirect', to: '/unconfirmed' });
  });

  it('missing permission → redirect to /', () => {
    expect(
      guardDecision(state({ data: { confirmed: true } }), { requirePermission: 0xff }, never),
    ).toEqual({ kind: 'redirect', to: '/' });
  });

  it('has permission → allow', () => {
    expect(
      guardDecision(state({ data: { confirmed: true } }), { requirePermission: 0x01 }, always),
    ).toEqual({ kind: 'allow' });
  });

  it('error takes precedence over an (absent) data value', () => {
    // isError true with data still undefined must not fall through to /login.
    expect(guardDecision(state({ isError: true, data: undefined }), {}, always).kind).toBe('error');
  });
});
