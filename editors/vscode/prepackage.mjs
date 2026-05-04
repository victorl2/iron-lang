#!/usr/bin/env node
// Phase 6 Plan 06-03 Task 1: copy grammars into editors/vscode/ before
// `vsce package`. The canonical TextMate grammar + language-configuration
// live in grammars/textmate/ and are drift-guarded by CTest
// test_grammar_keyword_drift_textmate; we regenerate copies here at
// prepackage time so the .vsix carries the latest drift-gated artifacts.
import { copyFileSync, existsSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const repo = join(__dirname, '..', '..');

const sources = [
  [
    join(repo, 'grammars/textmate/iron.tmLanguage.json'),
    join(__dirname, 'syntaxes/iron.tmLanguage.json'),
  ],
  [
    join(repo, 'grammars/textmate/language-configuration.json'),
    join(__dirname, 'language-configuration.json'),
  ],
];

for (const [src, dst] of sources) {
  if (!existsSync(src)) {
    console.error(`prepackage: missing source ${src}`);
    process.exit(1);
  }
  mkdirSync(dirname(dst), { recursive: true });
  copyFileSync(src, dst);
  console.log(`prepackage: copied ${src} -> ${dst}`);
}

console.log('prepackage: done');
