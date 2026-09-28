import { useState } from 'react';
import { Link, useLocation, useNavigate } from 'react-router-dom';
import { IconLogout, IconMoon, IconSun } from '@tabler/icons-react';

import { BRAND } from '@/lib/brand';
import { useLogout } from '@/hooks/useAuthMutations';
import { useMe } from '@/hooks/useMe';
import { applyTheme, readTheme, type Theme } from '@/lib/theme';
import { userCan } from '@/lib/auth/permissions';
import { routes, guardPermission, type RouteEntry } from '@/routes/manifest';

/**
 * Tabler's vertical navbar. The collapse is React state, not Bootstrap JS: the
 * project ships no Bootstrap bundle, so `show` is toggled by hand.
 */
export function Nav() {
  const me = useMe();
  const user = me.data ?? null;
  const logout = useLogout();
  const navigate = useNavigate();
  const location = useLocation();
  const [menuOpen, setMenuOpen] = useState(false);
  const [theme, setTheme] = useState<Theme>(() => readTheme());

  const toggleTheme = () => {
    const next: Theme = theme === 'dark' ? 'light' : 'dark';
    setTheme(next);
    applyTheme(next);
  };

  // Show logged-out auth links only once /me resolved to "no session".
  const showAuthButtons = me.isSuccess && !user;
  const navLinks: RouteEntry[] = routes.filter(
    (r) => r.navLabel && userCan(user, guardPermission(r)),
  );
  const isActive = (path: string) =>
    path === '/' ? location.pathname === '/' : location.pathname.startsWith(path);

  const logoutAndRedirect = async () => {
    await logout.mutateAsync();
    navigate('/login');
  };

  const themeLabel = theme === 'dark' ? 'Switch to light theme' : 'Switch to dark theme';

  return (
    <aside className="navbar navbar-vertical navbar-expand-lg">
      <div className="container-fluid">
        <button
          className="navbar-toggler"
          type="button"
          aria-label="Toggle navigation menu"
          aria-expanded={menuOpen}
          aria-controls="sidebar-menu"
          onClick={() => setMenuOpen((open) => !open)}
        >
          <span className="navbar-toggler-icon" />
        </button>
        <div className="navbar-brand navbar-brand-autodark">
          <Link to="/admin">{BRAND}</Link>
        </div>
        <div className="navbar-nav flex-row d-lg-none">
          <button
            className="nav-link px-0"
            type="button"
            onClick={toggleTheme}
            aria-label={themeLabel}
          >
            {theme === 'dark' ? <IconSun size={20} /> : <IconMoon size={20} />}
          </button>
        </div>
        <div
          id="sidebar-menu"
          className={menuOpen ? 'collapse navbar-collapse show' : 'collapse navbar-collapse'}
        >
          <ul className="navbar-nav pt-lg-3">
            {navLinks.map((entry) => (
              <li
                key={entry.path}
                className={isActive(entry.path) ? 'nav-item active' : 'nav-item'}
              >
                <Link
                  to={entry.path}
                  className="nav-link"
                  aria-current={isActive(entry.path) ? 'page' : undefined}
                  onClick={() => setMenuOpen(false)}
                >
                  <span className="nav-link-title">{entry.navLabel}</span>
                </Link>
              </li>
            ))}
          </ul>
          <div className="mt-auto pb-3">
            <ul className="navbar-nav">
              <li className="nav-item d-none d-lg-block">
                <button
                  className="nav-link"
                  type="button"
                  onClick={toggleTheme}
                  aria-label={themeLabel}
                >
                  <span className="nav-link-icon">
                    {theme === 'dark' ? <IconSun size={18} /> : <IconMoon size={18} />}
                  </span>
                  <span className="nav-link-title">
                    {theme === 'dark' ? 'Light theme' : 'Dark theme'}
                  </span>
                </button>
              </li>
              {user && (
                <>
                  <li className={isActive('/account') ? 'nav-item active' : 'nav-item'}>
                    <Link to="/account" className="nav-link" onClick={() => setMenuOpen(false)}>
                      <span className="nav-link-title">{user.full_name || user.email}</span>
                    </Link>
                  </li>
                  <li className="nav-item">
                    <button className="nav-link" type="button" onClick={logoutAndRedirect}>
                      <span className="nav-link-icon">
                        <IconLogout size={18} />
                      </span>
                      <span className="nav-link-title">Log out</span>
                    </button>
                  </li>
                </>
              )}
              {showAuthButtons && (
                <>
                  <li className="nav-item">
                    <Link to="/login" className="nav-link" onClick={() => setMenuOpen(false)}>
                      <span className="nav-link-title">Log in</span>
                    </Link>
                  </li>
                  <li className="nav-item">
                    <Link to="/register" className="nav-link" onClick={() => setMenuOpen(false)}>
                      <span className="nav-link-title">Register</span>
                    </Link>
                  </li>
                </>
              )}
            </ul>
          </div>
        </div>
      </div>
    </aside>
  );
}
