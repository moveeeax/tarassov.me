import { useState } from 'react';
import { Link, useLocation, useNavigate } from 'react-router-dom';
import { LogOut, Menu, Moon, Sun, X } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { BRAND } from '@/lib/brand';
import { useLogout } from '@/hooks/useAuthMutations';
import { useMe } from '@/hooks/useMe';
import { cn } from '@/lib/utils';
import { userCan } from '@/lib/auth/permissions';
import { routes, guardPermission, type RouteEntry } from '@/routes/manifest';

export function Nav() {
  const me = useMe();
  const user = me.data ?? null;
  const logout = useLogout();
  const navigate = useNavigate();
  const location = useLocation();
  const [menuOpen, setMenuOpen] = useState(false);

  // Minimal theme toggle: the .dark class drives Tailwind's dark: variants; the
  // initial class is set pre-paint by the inline script in index.html.
  const [dark, setDark] = useState(
    () => typeof document !== 'undefined' && document.documentElement.classList.contains('dark'),
  );
  const toggleTheme = () => {
    const next = !dark;
    setDark(next);
    document.documentElement.classList.toggle('dark', next);
    try {
      localStorage.setItem('theme', next ? 'dark' : 'light');
    } catch {
      /* ignore */
    }
  };

  // Show the logged-out auth buttons (Log in / Register) only once /me has
  // RESOLVED to "no session" — me.isSuccess && !user. Gating on isSuccess
  // (instead of just !isError) avoids the first-paint flash of "Log in /
  // Register" while /me is still pending, and still never claims the user
  // is logged out on a 5xx/network error (isSuccess stays false then).
  const showAuthButtons = me.isSuccess && !user;

  // Nav links come straight from the routes manifest — every route that
  // declares a navLabel, filtered by what this user is allowed to see.
  // One source of truth for routes and nav kills the route↔nav drift.
  const navLinks: RouteEntry[] = routes.filter(
    (r) => r.navLabel && userCan(user, guardPermission(r)),
  );

  // A route is active when the current path matches exactly, or is a child of
  // it (so /admin/users still highlights "Admin"). "/" matches only itself.
  const isActive = (path: string) =>
    path === '/' ? location.pathname === '/' : location.pathname.startsWith(path);

  const logoutAndRedirect = async () => {
    await logout.mutateAsync();
    navigate('/login');
  };

  return (
    <nav className="border-b border-border bg-background">
      <div className="container mx-auto flex h-14 items-center justify-between">
        <div className="flex items-center gap-6">
          <Link
            to="/"
            className="font-semibold rounded focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 focus-visible:ring-offset-background"
          >
            {BRAND}
          </Link>
          {/* Desktop nav cluster */}
          <div className="hidden items-center gap-4 text-sm md:flex">
            {navLinks.map((r) => {
              const Icon = r.navIcon;
              return (
                <Link
                  key={r.path}
                  to={r.path}
                  aria-current={isActive(r.path) ? 'page' : undefined}
                  className={cn(
                    'flex items-center gap-1 rounded transition-colors hover:text-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 focus-visible:ring-offset-background',
                    isActive(r.path) ? 'font-medium text-primary' : 'text-muted-foreground',
                  )}
                >
                  {Icon && <Icon className="h-3.5 w-3.5" />}
                  {r.navLabel}
                </Link>
              );
            })}
          </div>
        </div>
        <div className="flex items-center gap-3 text-sm">
          <Button
            size="sm"
            variant="ghost"
            onClick={toggleTheme}
            aria-label={dark ? 'Switch to light theme' : 'Switch to dark theme'}
          >
            {dark ? <Sun className="h-4 w-4" /> : <Moon className="h-4 w-4" />}
          </Button>
          {/* Desktop account cluster */}
          <div className="hidden items-center gap-3 md:flex">
            {user && (
              <>
                <Link
                  to="/account"
                  aria-current={isActive('/account') ? 'page' : undefined}
                  className={cn(
                    'rounded hover:text-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 focus-visible:ring-offset-background',
                    isActive('/account') ? 'text-primary' : 'text-muted-foreground',
                  )}
                >
                  {user.full_name || user.email}
                </Link>
                <Button size="sm" variant="ghost" aria-label="Log out" onClick={logoutAndRedirect}>
                  <LogOut className="h-4 w-4" />
                </Button>
              </>
            )}
            {showAuthButtons && (
              <>
                <Button size="sm" variant="ghost" asChild>
                  <Link to="/login">Log in</Link>
                </Button>
                <Button size="sm" asChild>
                  <Link to="/register">Register</Link>
                </Button>
              </>
            )}
          </div>
          {/* Mobile disclosure toggle */}
          <Button
            size="sm"
            variant="ghost"
            className="md:hidden"
            aria-label="Toggle navigation menu"
            aria-expanded={menuOpen}
            aria-controls="mobile-nav"
            onClick={() => setMenuOpen((o) => !o)}
          >
            {menuOpen ? <X className="h-4 w-4" /> : <Menu className="h-4 w-4" />}
          </Button>
        </div>
      </div>

      {/* Mobile stacked panel */}
      {menuOpen && (
        <div id="mobile-nav" className="border-t border-border md:hidden">
          <div className="container mx-auto flex flex-col gap-1 py-3 text-sm">
            {navLinks.map((r) => {
              const Icon = r.navIcon;
              return (
                <Link
                  key={r.path}
                  to={r.path}
                  aria-current={isActive(r.path) ? 'page' : undefined}
                  onClick={() => setMenuOpen(false)}
                  className={cn(
                    'flex items-center gap-2 rounded px-2 py-2 hover:bg-accent focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring',
                    isActive(r.path) ? 'font-medium text-primary' : 'text-muted-foreground',
                  )}
                >
                  {Icon && <Icon className="h-4 w-4" />}
                  {r.navLabel}
                </Link>
              );
            })}
            {user && (
              <>
                <Link
                  to="/account"
                  aria-current={isActive('/account') ? 'page' : undefined}
                  onClick={() => setMenuOpen(false)}
                  className={cn(
                    'rounded px-2 py-2 hover:bg-accent focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring',
                    isActive('/account') ? 'font-medium text-primary' : 'text-muted-foreground',
                  )}
                >
                  {user.full_name || user.email}
                </Link>
                <Button
                  variant="ghost"
                  className="justify-start px-2"
                  aria-label="Log out"
                  onClick={() => {
                    setMenuOpen(false);
                    logoutAndRedirect();
                  }}
                >
                  <LogOut className="mr-2 h-4 w-4" />
                  Log out
                </Button>
              </>
            )}
            {showAuthButtons && (
              <div className="flex flex-col gap-2 px-2 pt-2">
                <Button variant="ghost" asChild>
                  <Link to="/login" onClick={() => setMenuOpen(false)}>
                    Log in
                  </Link>
                </Button>
                <Button asChild>
                  <Link to="/register" onClick={() => setMenuOpen(false)}>
                    Register
                  </Link>
                </Button>
              </div>
            )}
          </div>
        </div>
      )}
    </nav>
  );
}
