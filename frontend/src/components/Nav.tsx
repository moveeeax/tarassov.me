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

const focusRing =
  'rounded focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 focus-visible:ring-offset-background';

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

  // Show logged-out auth buttons only once /me resolved to "no session".
  // Gating on isSuccess avoids the first-paint flash of "Log in / Register".
  const showAuthButtons = me.isSuccess && !user;

  const navLinks: RouteEntry[] = routes.filter(
    (r) => r.navLabel && userCan(user, guardPermission(r)),
  );

  // Active when path matches exactly, or is a child (so /admin/users highlights Admin).
  const isActive = (path: string) =>
    path === '/' ? location.pathname === '/' : location.pathname.startsWith(path);

  const logoutAndRedirect = async () => {
    await logout.mutateAsync();
    navigate('/login');
  };

  const closeMenu = () => setMenuOpen(false);

  return (
    <nav className="border-b border-border bg-background">
      <div className="container mx-auto flex h-14 items-center justify-between">
        <div className="flex items-center gap-6">
          <Link to="/" className={cn('font-semibold', focusRing)}>
            {BRAND}
          </Link>
          <div className="hidden items-center gap-4 text-sm md:flex">
            {navLinks.map((r) => (
              <ManifestLink key={r.path} entry={r} active={isActive(r.path)} variant="desktop" />
            ))}
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
          <div className="hidden items-center gap-3 md:flex">
            {user && (
              <>
                <AccountLink
                  label={user.full_name || user.email}
                  active={isActive('/account')}
                  variant="desktop"
                />
                <Button size="sm" variant="ghost" aria-label="Log out" onClick={logoutAndRedirect}>
                  <LogOut className="h-4 w-4" />
                </Button>
              </>
            )}
            {showAuthButtons && <AuthButtons />}
          </div>
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

      {menuOpen && (
        <div id="mobile-nav" className="border-t border-border md:hidden">
          <div className="container mx-auto flex flex-col gap-1 py-3 text-sm">
            {navLinks.map((r) => (
              <ManifestLink
                key={r.path}
                entry={r}
                active={isActive(r.path)}
                variant="mobile"
                onNavigate={closeMenu}
              />
            ))}
            {user && (
              <>
                <AccountLink
                  label={user.full_name || user.email}
                  active={isActive('/account')}
                  variant="mobile"
                  onNavigate={closeMenu}
                />
                <Button
                  variant="ghost"
                  className="justify-start px-2"
                  aria-label="Log out"
                  onClick={() => {
                    closeMenu();
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
                <AuthButtons onNavigate={closeMenu} stacked />
              </div>
            )}
          </div>
        </div>
      )}
    </nav>
  );
}

type LinkVariant = 'desktop' | 'mobile';

function ManifestLink({
  entry,
  active,
  variant,
  onNavigate,
}: {
  entry: RouteEntry;
  active: boolean;
  variant: LinkVariant;
  onNavigate?: () => void;
}) {
  const Icon = entry.navIcon;
  return (
    <Link
      to={entry.path}
      aria-current={active ? 'page' : undefined}
      onClick={onNavigate}
      className={cn(
        variant === 'desktop'
          ? 'flex items-center gap-1 transition-colors hover:text-foreground'
          : 'flex items-center gap-2 px-2 py-2 hover:bg-accent',
        focusRing,
        active ? 'font-medium text-primary' : 'text-muted-foreground',
      )}
    >
      {Icon && <Icon className={variant === 'desktop' ? 'h-3.5 w-3.5' : 'h-4 w-4'} />}
      {entry.navLabel}
    </Link>
  );
}

function AccountLink({
  label,
  active,
  variant,
  onNavigate,
}: {
  label: string;
  active: boolean;
  variant: LinkVariant;
  onNavigate?: () => void;
}) {
  return (
    <Link
      to="/account"
      aria-current={active ? 'page' : undefined}
      onClick={onNavigate}
      className={cn(
        variant === 'desktop' ? 'hover:text-foreground' : 'px-2 py-2 hover:bg-accent',
        focusRing,
        active
          ? variant === 'desktop'
            ? 'text-primary'
            : 'font-medium text-primary'
          : 'text-muted-foreground',
      )}
    >
      {label}
    </Link>
  );
}

function AuthButtons({
  onNavigate,
  stacked,
}: {
  onNavigate?: () => void;
  stacked?: boolean;
}) {
  const size = stacked ? undefined : ('sm' as const);
  return (
    <>
      <Button size={size} variant="ghost" asChild>
        <Link to="/login" onClick={onNavigate}>
          Log in
        </Link>
      </Button>
      <Button size={size} asChild>
        <Link to="/register" onClick={onNavigate}>
          Register
        </Link>
      </Button>
    </>
  );
}
