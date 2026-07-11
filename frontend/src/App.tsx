import { Suspense, type ReactNode } from 'react';
import { Routes, Route, Outlet } from 'react-router-dom';

import { Layout } from '@/components/Layout';
import { ProtectedRoute } from '@/components/ProtectedRoute';
import { NotFoundPage } from '@/pages/NotFound';
import { Permission } from '@/lib/auth/permissions';
import { routes, type RouteEntry, type RouteGuard } from '@/routes/manifest';

// Fallback shown while a code-split admin chunk loads. Matches the plain
// "Loading…" the guards already use, so the transition is visually quiet.
const ChunkFallback = (
  <div className="container mx-auto py-8 text-muted-foreground">Loading…</div>
);

/**
 * Guard groups as layout routes: the wrapper renders once and children
 * mount via <Outlet/>. A new admin page added under guard:'admin' can't
 * accidentally skip the permission check.
 */
function GuardLayout({
  requirePermission,
  requireConfirmed,
  suspense,
}: {
  requirePermission?: number;
  requireConfirmed?: boolean;
  suspense?: boolean;
}) {
  const body: ReactNode = suspense ? (
    <Suspense fallback={ChunkFallback}>
      <Outlet />
    </Suspense>
  ) : (
    <Outlet />
  );
  return (
    <ProtectedRoute requirePermission={requirePermission} requireConfirmed={requireConfirmed}>
      {body}
    </ProtectedRoute>
  );
}

function routesFor(guard: RouteGuard): RouteEntry[] {
  return routes.filter((r) => r.guard === guard);
}

function renderRoute(r: RouteEntry) {
  return (
    <Route
      key={r.path}
      path={r.path}
      element={
        r.requirePermission !== undefined && r.guard !== 'admin' ? (
          <ProtectedRoute requirePermission={r.requirePermission} requireConfirmed>
            {r.element}
          </ProtectedRoute>
        ) : (
          r.element
        )
      }
    />
  );
}

export default function App() {
  return (
    <Routes>
      <Route element={<Layout />}>
        {routesFor('public').map(renderRoute)}

        <Route element={<GuardLayout />}>{routesFor('auth').map(renderRoute)}</Route>

        <Route element={<GuardLayout requireConfirmed suspense />}>
          {routesFor('confirmed').map(renderRoute)}
        </Route>

        <Route
          element={
            <GuardLayout
              requirePermission={Permission.Administer}
              requireConfirmed
              suspense
            />
          }
        >
          {routesFor('admin').map(renderRoute)}
        </Route>

        <Route path="*" element={<NotFoundPage />} />
      </Route>
    </Routes>
  );
}
