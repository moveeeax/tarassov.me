import js from '@eslint/js';
import globals from 'globals';
import reactHooks from 'eslint-plugin-react-hooks';
import reactRefresh from 'eslint-plugin-react-refresh';
import tseslint from 'typescript-eslint';

export default tseslint.config(
  // schema.gen.ts is (re)generated from docs/openapi.yaml — not ours to lint.
  { ignores: ['dist', 'src/lib/api/schema.gen.ts'] },
  {
    files: ['**/*.{ts,tsx}'],
    extends: [js.configs.recommended, ...tseslint.configs.recommended],
    languageOptions: {
      ecmaVersion: 2022,
      globals: globals.browser,
    },
    plugins: {
      'react-hooks': reactHooks,
      'react-refresh': reactRefresh,
    },
    rules: {
      ...reactHooks.configs.recommended.rules,
      // allowConstantExport: shadcn primitives export cva() consts
      // (e.g. buttonVariants) next to the component — fine for HMR.
      'react-refresh/only-export-components': ['warn', { allowConstantExport: true }],
      '@typescript-eslint/no-unused-vars': ['warn', { argsIgnorePattern: '^_' }],
    },
  },
  // Plain-JS config files at the repo root (tailwind, postcss, this file).
  {
    files: ['*.js'],
    extends: [js.configs.recommended],
    languageOptions: {
      globals: { ...globals.node, require: 'readonly' },
    },
  },
);
