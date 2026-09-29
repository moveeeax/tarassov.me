import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { WIDGET_COMPONENTS } from './registry';

/**
 * Cross-language drift guard for the widget catalog.
 *
 * Spec §5.4 exists so there is ONE list of widget types: the constexpr array in
 * src/domain/WidgetCatalog.hpp, which both publishes the catalog and validates
 * writes. A hardcoded copy of that list here would be a third one — green while
 * a type added on the backend renders "Unknown widget" in production.
 *
 * So the list is PARSED out of the header, the same technique
 * permissions.drift.test.ts uses on Role.hpp. Add an entry to kCatalog without a
 * component and this goes red.
 *
 * Deliberately NOT under the jsdom environment: this reads a file off disk
 * through import.meta.url, and jsdom turns that into an http URL, which breaks
 * fileURLToPath.
 */

const CATALOG_HPP = fileURLToPath(
  new URL('../../../src/domain/WidgetCatalog.hpp', import.meta.url),
);

/**
 * Pull the widget type strings out of kCatalog's initialiser. Each entry starts
 * with its type as the first string literal on the line, e.g.
 *   {"posts_summary",
 *   {"service_health", "Service", …},
 * The slice is bounded by the kCatalog array so OptionSpec keys ("limit") and
 * the kNoOptions placeholder cannot leak in.
 */
function parseCatalogTypes(src: string): string[] {
  const start = src.indexOf('kCatalog{{');
  expect(start, 'kCatalog initialiser not found in WidgetCatalog.hpp').toBeGreaterThan(-1);
  const end = src.indexOf('}};', start);
  const block = src.slice(start, end);
  const types: string[] = [];
  for (const line of block.split('\n')) {
    const m = /^\s*\{"([a-z_]+)"/.exec(line);
    if (m) types.push(m[1]);
  }
  return types;
}

describe('widget registry mirrors Domain::Widgets::kCatalog', () => {
  const backendTypes = parseCatalogTypes(readFileSync(CATALOG_HPP, 'utf8'));

  it('parsed the C++ catalog (sanity)', () => {
    // If the header moves or the array style changes, fail loudly here rather
    // than silently comparing two empty lists.
    expect(backendTypes.length).toBeGreaterThanOrEqual(5);
    expect(backendTypes).toContain('posts_summary');
  });

  it('every backend type has a component', () => {
    for (const type of backendTypes) {
      expect(WIDGET_COMPONENTS[type], `no component registered for ${type}`).toBeDefined();
    }
  });

  it('no component exists without a backend type', () => {
    expect(Object.keys(WIDGET_COMPONENTS).sort()).toEqual([...backendTypes].sort());
  });
});
