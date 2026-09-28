import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

/**
 * Dead-class guard for the Tabler conversion.
 *
 * Removing Tailwind turned every leftover utility class into a no-op that still
 * looks deliberate in the source: `text-right`, `hidden`, `text-green-600` and
 * friends silently stop doing anything instead of breaking loudly. This walks
 * every className in the tree and asserts each token exists in a stylesheet the
 * app actually loads.
 *
 * Sources of truth: @tabler/core's dist CSS, react-grid-layout's sheet, and our
 * own src/index.css.
 *
 * Node environment on purpose (no `@vitest-environment jsdom`): it reads files
 * off disk through import.meta.url, and jsdom turns that URL into http scheme,
 * which breaks fileURLToPath.
 */

const here = (rel: string) => fileURLToPath(new URL(rel, import.meta.url));

/**
 * Every .tsx under a directory. A hand-rolled walk rather than fs.globSync:
 * that landed in Node 22 and CI's frontend job runs Node 20
 * (.github/workflows/ci.yml), so globSync passes locally and throws there.
 */
function tsxFilesUnder(dir: string): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) out.push(...tsxFilesUnder(path));
    else if (entry.name.endsWith('.tsx')) out.push(path);
  }
  return out;
}

/** Class tokens defined by the stylesheets the app loads. */
function knownClasses(): Set<string> {
  const sheets = [
    here('../../node_modules/@tabler/core/dist/css/tabler.css'),
    here('../../node_modules/react-grid-layout/css/styles.css'),
    here('../index.css'),
  ];
  const known = new Set<string>();
  for (const sheet of sheets) {
    for (const m of readFileSync(sheet, 'utf8').matchAll(/\.(-?[a-zA-Z_][\w-]*)/g)) {
      known.add(m[1]);
    }
  }
  return known;
}

/**
 * Every class token written in the tree. Covers the four shapes the codebase
 * uses: a literal attribute, a template literal, `cn()` arguments, a
 * `className:` object property (the DataTable column definitions), and both arms
 * of a ternary — that last one is where `text-green-600` hid from the first
 * version of this scan.
 */
const CLASS_PATTERNS = [
  /className="([^"]*)"/g,
  /className=\{`([^`]*)`\}/g,
  /className=\{cn\(([^)]*)\)/g,
  /className:\s*'([^']*)'/g,
  /className:\s*"([^"]*)"/g,
  /className:\s*`([^`]*)`/g,
  /className=\{[^}]*\?\s*'([^']*)'/g,
  /className=\{[^}]*:\s*'([^']*)'\}/g,
];

/** Names that are JS expressions inside cn(), not class literals. */
const NOT_A_CLASS = /^[A-Z_]+\[|^(className|size|variant|invalid)$/;

function tokensIn(source: string): string[] {
  const out: string[] = [];
  for (const pattern of CLASS_PATTERNS) {
    for (const m of source.matchAll(pattern)) {
      const raw = (m[1] ?? '').replace(/\$\{[^}]*\}/g, ' ').replace(/['",]/g, ' ');
      for (const token of raw.split(/\s+/)) {
        if (!token || NOT_A_CLASS.test(token)) continue;
        if (!/^[a-zA-Z_-][\w:./[\]%-]*$/.test(token)) continue;
        out.push(token);
      }
    }
  }
  return out;
}

describe('every class in the tree is defined by a stylesheet we load', () => {
  const known = knownClasses();

  it('parsed the stylesheets (sanity)', () => {
    // Fail loudly if a sheet moves, rather than passing an empty comparison.
    expect(known.size).toBeGreaterThan(500);
    for (const staple of ['card', 'btn', 'form-control', 'react-grid-placeholder']) {
      expect(known, `stylesheet parse missed .${staple}`).toContain(staple);
    }
  });

  it('has no unknown class tokens', () => {
    const files = tsxFilesUnder(here('..'));
    expect(files.length).toBeGreaterThan(20);
    const unknown: Record<string, string[]> = {};
    for (const file of files) {
      for (const token of tokensIn(readFileSync(file, 'utf8'))) {
        if (known.has(token)) continue;
        (unknown[token] ??= []).push(file.replace(/.*\/src\//, 'src/'));
      }
    }
    expect(unknown).toEqual({});
  });
});
