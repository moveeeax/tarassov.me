import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { createContext, runInContext } from 'node:vm';

import { describe, expect, it } from 'vitest';

/**
 * The public blog's code blocks (frontend/public-site/js/code-blocks.js) are
 * plain browser scripts, not part of the SPA bundle. Run them the way the
 * article page does — the vendored Prism bundle first, then code-blocks.js —
 * in a bare VM context and check the pieces that can silently rot: the alias
 * table must point at grammars that are actually in prism.min.js, and the
 * fence-name parsing must match what marked emits (class="language-<name>").
 */

const SITE_JS = (name: string) =>
  fileURLToPath(new URL(`../../public-site/js/${name}`, import.meta.url));

type Grammar = Record<string, unknown>;
interface BlogCode {
  ALIASES: Record<string, string>;
  languageOf(className: string): string;
  prismLanguage(lang: string, languages: Record<string, unknown>): string;
}
interface Prism {
  languages: Record<string, Grammar | unknown>;
  highlight(code: string, grammar: Grammar, language: string): string;
}

function load(): { prism: Prism; code: BlogCode } {
  const ctx = createContext({});
  runInContext(readFileSync(SITE_JS('prism.min.js'), 'utf8'), ctx);
  runInContext(readFileSync(SITE_JS('code-blocks.js'), 'utf8'), ctx);
  return { prism: ctx.Prism as Prism, code: ctx.BlogCode as BlogCode };
}

const { prism, code } = load();

describe('public-site code blocks', () => {
  it('bundles the grammars the blog posts use', () => {
    for (const id of ['hcl', 'yaml', 'bash', 'json', 'go', 'python', 'cpp', 'sql', 'docker']) {
      expect(typeof prism.languages[id], id).toBe('object');
    }
  });

  it('maps every alias to a bundled grammar', () => {
    for (const [alias, id] of Object.entries(code.ALIASES)) {
      expect(typeof prism.languages[id], `${alias} -> ${id}`).toBe('object');
    }
  });

  it('reads the fence language from the class marked emits', () => {
    expect(code.languageOf('language-hcl')).toBe('hcl');
    expect(code.languageOf('foo language-Terraform bar')).toBe('terraform');
    expect(code.languageOf('lang-yaml')).toBe('yaml');
    expect(code.languageOf('')).toBe('');
    expect(code.languageOf('hljs')).toBe('');
  });

  it('resolves aliases and rejects unknown or plain languages', () => {
    expect(code.prismLanguage('terraform', prism.languages)).toBe('hcl');
    expect(code.prismLanguage('yml', prism.languages)).toBe('yml');
    expect(code.prismLanguage('console', prism.languages)).toBe('shell-session');
    expect(code.prismLanguage('brainfuck', prism.languages)).toBe('');
    expect(code.prismLanguage('text', prism.languages)).toBe('');
    expect(code.prismLanguage('', prism.languages)).toBe('');
  });

  it('highlights Terraform through the hcl grammar', () => {
    const id = code.prismLanguage('terraform', prism.languages);
    const html = prism.highlight(
      'resource "aws_s3_bucket" "b" {\n  bucket = "x"\n}',
      prism.languages[id] as Grammar,
      id,
    );
    expect(html).toContain('<span class="token keyword">resource');
    expect(html).toContain('<span class="token property">bucket</span>');
  });
});
