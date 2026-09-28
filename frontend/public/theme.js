// Dark stays this app's default (Tabler's own default is light); only an
// explicit 'light' choice opts out. Runs before first paint to avoid a flash.
// Served as a static /theme.js asset so the production CSP (script-src 'self',
// no inline) covers it.
try {
  document.documentElement.setAttribute(
    'data-bs-theme',
    localStorage.getItem('theme') === 'light' ? 'light' : 'dark',
  );
} catch (e) {
  document.documentElement.setAttribute('data-bs-theme', 'dark');
}
