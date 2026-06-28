import { type ReactNode } from 'react';
import { Navigate, useLocation } from 'react-router-dom';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { useMe } from '@/hooks/useMe';
import { apiErrorMessage } from '@/lib/api/client';
import { userCan } from '@/lib/auth/permissions';
import type { User } from '@/lib/api/types';

interface ProtectedRouteProps {
  children: ReactNode;
  /** Required permission bit (defaults to "any authenticated user"). */
  requirePermission?: number;
  /** When true, redirect logged-in-but-unconfirmed users to /unconfirmed. */
  requireConfirmed?: boolean;
}

/** Minimal shape of the useMe() query result the guard decision depends on. */
export interface GuardSessionState {
  isPending: boolean;
  isError: boolean;
  /**
   * Resolved user, or null on a 401 (logged out). Narrowed to just the
   * fields the guard reads (`confirmed` for the unconfirmed redirect, `role`
   * for the permission check via userCan) so no `as never` cast is needed.
   */
  data: Pick<User, 'confirmed' | 'role'> | null | undefined;
}

export type GuardDecision =
  | { kind: 'loading' }
  | { kind: 'error' }
  | { kind: 'redirect'; to: '/login' | '/unconfirmed' | '/' }
  | { kind: 'allow' };

/**
 * Pure routing decision for ProtectedRoute, extracted so it can be unit
 * tested without rendering a React tree (no @testing-library in the stack).
 *
 * The critical invariant the round-2 audit cares about:
 *   - isError  → 'error' (show retry), NEVER a /login redirect
 *   - data===null (401) → redirect to /login
 */
export function guardDecision(
  me: GuardSessionState,
  opts: { requirePermission?: number; requireConfirmed?: boolean },
  hasPermission: (user: NonNullable<GuardSessionState['data']>, bit: number) => boolean,
): GuardDecision {
  if (me.isPending) return { kind: 'loading' };
  // A thrown error is a *real* failure (network / 5xx); 401 resolves to
  // null instead. Never bounce a possibly-logged-in user to /login here.
  if (me.isError) return { kind: 'error' };

  const user = me.data ?? null;
  if (!user) return { kind: 'redirect', to: '/login' };
  if (opts.requireConfirmed && !user.confirmed) return { kind: 'redirect', to: '/unconfirmed' };
  if (opts.requirePermission !== undefined && !hasPermission(user, opts.requirePermission)) {
    return { kind: 'redirect', to: '/' };
  }
  return { kind: 'allow' };
}

/**
 * Wraps a page with auth guards:
 *   - Not logged in → /login (with `from` state for redirect-after-login)
 *   - Missing permission → /
 *   - Unconfirmed (and requireConfirmed) → /unconfirmed
 *
 * Reads the user directly from the useMe() query — the TanStack Query
 * cache is the single source of truth for the session.
 */
export function ProtectedRoute({ children, requirePermission, requireConfirmed }: ProtectedRouteProps) {
  const location = useLocation();
  const me = useMe();

  const decision = guardDecision(
    me,
    { requirePermission, requireConfirmed },
    (user, bit) => userCan(user, bit),
  );

  switch (decision.kind) {
    case 'loading':
      // isPending = no data and no error yet. Distinct from isFetching,
      // which can flip true on background revalidations of cached data.
      return <div className="container mx-auto py-8 text-muted-foreground">Loading…</div>;
    case 'error':
      // A thrown error from useMe is a *real* failure (network / 5xx) — the
      // 401 "no session" case resolves to null instead. Don't bounce a
      // possibly-logged-in user to /login on a transient blip; let them retry.
      return (
        <div className="container mx-auto max-w-md py-8 space-y-4">
          <Alert variant="destructive">
            <AlertDescription>
              {apiErrorMessage(me.error, 'Could not load your session.')}
            </AlertDescription>
          </Alert>
          <Button onClick={() => me.refetch()} disabled={me.isFetching}>
            {me.isFetching ? 'Retrying…' : 'Retry'}
          </Button>
        </div>
      );
    case 'redirect':
      return (
        <Navigate
          to={decision.to}
          replace
          state={decision.to === '/login' ? { from: location.pathname } : undefined}
        />
      );
    case 'allow':
      return <>{children}</>;
  }
}
