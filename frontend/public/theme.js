// Dark is the default; only an explicit 'light' choice opts out. Applied
// before first paint to avoid a flash. Served as a static /theme.js asset so
// the production CSP (script-src 'self', no inline) doesn't block it.
try {
  if (localStorage.getItem('theme') !== 'light') {
    document.documentElement.classList.add('dark');
  }
} catch (e) {
  document.documentElement.classList.add('dark');
}
