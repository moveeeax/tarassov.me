import { Outlet } from 'react-router-dom';

import { Nav } from './Nav';
import { useMe } from '@/hooks/useMe';

/**
 * Top-level shell. Calling useMe here means every page below has a
 * fresh principal in the TanStack Query cache on first paint.
 */
export function Layout() {
  useMe();
  return (
    <div className="min-h-screen flex flex-col">
      <a
        href="#main-content"
        className="sr-only focus:not-sr-only focus:absolute focus:left-4 focus:top-4 focus:z-50 focus:rounded focus:bg-background focus:px-3 focus:py-2 focus:text-sm focus:font-medium focus:shadow focus-visible:ring-2 focus-visible:ring-ring"
      >
        Skip to main content
      </a>
      <Nav />
      <main id="main-content" className="flex-1">
        <Outlet />
      </main>
    </div>
  );
}
