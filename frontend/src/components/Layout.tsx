import { Outlet } from 'react-router-dom';

import { Nav } from './Nav';
import { useMe } from '@/hooks/useMe';

/**
 * Tabler page shell: a vertical navbar beside a .page-wrapper whose .page-body
 * holds the routed page. Calling useMe here means every page below has a fresh
 * principal in the TanStack Query cache on first paint.
 */
export function Layout() {
  useMe();
  return (
    <div className="page">
      <a href="#main-content" className="visually-hidden-focusable">
        Skip to main content
      </a>
      <Nav />
      <div className="page-wrapper">
        <main id="main-content" className="page-body">
          <Outlet />
        </main>
      </div>
    </div>
  );
}
