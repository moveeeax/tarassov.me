/**
 * Theme state. Tabler keys its dark palette off `[data-bs-theme="dark"]` on
 * <html>; its own default is LIGHT, so the pre-paint script in /theme.js sets
 * the attribute explicitly to keep this app's dark default.
 */
export type Theme = 'light' | 'dark';

export function readTheme(): Theme {
  return document.documentElement.getAttribute('data-bs-theme') === 'light' ? 'light' : 'dark';
}

export function applyTheme(next: Theme): void {
  document.documentElement.setAttribute('data-bs-theme', next);
  try {
    // window.localStorage, not the bare global: under Node 22 the bare name
    // resolves to Node's own experimental localStorage, which is unavailable
    // without a CLI flag, so a bare reference cannot be tested. Reading the
    // property can itself throw (Safari private mode), so it lives in the try.
    window.localStorage.setItem('theme', next);
  } catch {
    // Blocked or unavailable storage: the theme still switches for this tab.
  }
}
