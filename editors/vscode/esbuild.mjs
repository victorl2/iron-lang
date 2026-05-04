// Phase 6 Plan 06-03 Task 1: esbuild bundler for the VSCode extension.
// Produces a single dist/extension.js consumed by VSCode per package.json
// `main`. Keeps the .vsix small (CI asserts < 5 MB) per RESEARCH Pitfall 10.
import { build } from 'esbuild';

await build({
  entryPoints: ['src/extension.ts'],
  bundle: true,
  outfile: 'dist/extension.js',
  external: ['vscode'],
  format: 'cjs',
  platform: 'node',
  target: 'node20',
  sourcemap: true,
  minify: process.env.NODE_ENV === 'production',
  logLevel: 'info',
});
