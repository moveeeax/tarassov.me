// Dark stays this app's default (Tabler's own default is light); only an
// explicit 'light' choice opts out. Runs before first paint to avoid a flash.
// Served as a static /theme.js asset so the production CSP (script-src 'self',
// no inline) covers it.
// The `dark` class is transitional — the shadcn variables in index.css still
// key off it. Remove that line together with Tailwind (task 13).
try {
  var theme = localStorage.getItem('theme') === 'light' ? 'light' : 'dark';
  document.documentElement.setAttribute('data-bs-theme', theme);
  if (theme === 'dark') document.documentElement.classList.add('dark');
} catch (e) {
  document.documentElement.setAttribute('data-bs-theme', 'dark');
  document.documentElement.classList.add('dark');
}
