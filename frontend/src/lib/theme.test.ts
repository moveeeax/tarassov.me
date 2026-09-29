// @vitest-environment jsdom
//
// jsdom is opted in per file, not set globally: permissions.drift.test.ts reads
// src/domain/Role.hpp off disk through import.meta.url, and under jsdom that URL
// is http-scheme, which breaks fileURLToPath.
import { describe, expect, it, beforeEach } from 'vitest';

import { applyTheme, readTheme } from './theme';

/**
 * In-memory Storage double.
 *
 * Inside vitest's jsdom environment `window` IS globalThis, and Node 22 defines
 * its own `localStorage` accessor there which returns undefined unless the
 * process was started with --localstorage-file. That accessor shadows jsdom's
 * implementation (`sessionStorage` survives, `localStorage` does not), so a test
 * that wants to observe what the theme wrote has to supply the storage itself.
 */
function installStorage(overrides: Partial<Storage> = {}): Storage {
  const store = new Map<string, string>();
  const storage: Storage = {
    getItem: (key) => store.get(key) ?? null,
    setItem: (key, value) => {
      store.set(key, value);
    },
    removeItem: (key) => {
      store.delete(key);
    },
    clear: () => store.clear(),
    key: (index) => [...store.keys()][index] ?? null,
    get length() {
      return store.size;
    },
    ...overrides,
  };
  Object.defineProperty(window, 'localStorage', {
    value: storage,
    configurable: true,
    writable: true,
  });
  return storage;
}

describe('theme', () => {
  beforeEach(() => {
    document.documentElement.removeAttribute('data-bs-theme');
    installStorage();
  });

  it('defaults to dark when nothing is set', () => {
    expect(readTheme()).toBe('dark');
  });

  it('reads the attribute the pre-paint script set', () => {
    document.documentElement.setAttribute('data-bs-theme', 'light');
    expect(readTheme()).toBe('light');
  });

  it('applies the attribute and the stored value', () => {
    applyTheme('light');
    expect(document.documentElement.getAttribute('data-bs-theme')).toBe('light');
    expect(window.localStorage.getItem('theme')).toBe('light');

    applyTheme('dark');
    expect(document.documentElement.getAttribute('data-bs-theme')).toBe('dark');
    expect(window.localStorage.getItem('theme')).toBe('dark');
  });

  // Private-mode Safari throws on setItem. The theme must still switch.
  it('survives storage that throws', () => {
    installStorage({
      setItem: () => {
        throw new Error('QuotaExceededError');
      },
    });
    expect(() => applyTheme('light')).not.toThrow();
    expect(document.documentElement.getAttribute('data-bs-theme')).toBe('light');
  });
});
