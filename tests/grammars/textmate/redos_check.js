#!/usr/bin/env node
// Phase 6 Plan 06-01 Task 2 (EXT-01, CONTEXT D-02, PITFALLS §20).
//
// Walks every "match", "begin", "end" regex string in the TextMate grammar
// at argv[2] and invokes safe-regex (https://www.npmjs.com/package/safe-regex).
// Exits 0 when the grammar is ReDoS-clean; exits 1 with a list of flagged
// patterns when any regex is vulnerable to catastrophic backtracking.
//
// Invoked by CTest test_grammar_redos_clean (phase-m5-invariant label).

'use strict';

const fs = require('node:fs');
const path = require('node:path');

function loadSafeRegex() {
  try {
    return require('safe-regex');
  } catch (err) {
    console.error('error: safe-regex not installed. Run `npm install --prefix '
      + path.dirname(__filename) + '` first.');
    console.error('       The CTest `test_grammars_npm_install` fixture performs this step.');
    console.error('       Underlying error:', err.message);
    process.exit(2);
  }
}

function walkRegexes(node, acc, keyPath) {
  if (node === null || node === undefined) return;
  if (typeof node === 'string') return;
  if (Array.isArray(node)) {
    node.forEach((child, idx) => walkRegexes(child, acc, `${keyPath}[${idx}]`));
    return;
  }
  if (typeof node === 'object') {
    for (const [key, value] of Object.entries(node)) {
      const childPath = keyPath ? `${keyPath}.${key}` : key;
      if ((key === 'match' || key === 'begin' || key === 'end')
          && typeof value === 'string') {
        acc.push({ pattern: value, path: childPath });
      } else {
        walkRegexes(value, acc, childPath);
      }
    }
  }
}

function main() {
  const grammarPath = process.argv[2];
  if (!grammarPath) {
    console.error('usage: node redos_check.js <path/to/grammar.json>');
    process.exit(2);
  }

  let grammar;
  try {
    grammar = JSON.parse(fs.readFileSync(grammarPath, 'utf8'));
  } catch (err) {
    console.error(`error: could not read/parse ${grammarPath}: ${err.message}`);
    process.exit(2);
  }

  const safeRegex = loadSafeRegex();
  const patterns = [];
  walkRegexes(grammar, patterns, '');

  if (patterns.length === 0) {
    console.error('error: no regex patterns found in grammar — did the extractor run?');
    process.exit(2);
  }

  const flagged = [];
  for (const entry of patterns) {
    // safe-regex treats its input as a regex source. Convert the JSON
    // string (which already contains raw regex metacharacters) verbatim.
    // safe-regex returns `true` for safe, `false` for catastrophic.
    let ok;
    try {
      ok = safeRegex(entry.pattern, { limit: 25 });
    } catch (err) {
      // safe-regex throws on some invalid regex inputs (unbalanced groups
      // etc.); treat as flagged so a reviewer looks at it.
      flagged.push({ ...entry, reason: `safe-regex threw: ${err.message}` });
      continue;
    }
    if (!ok) {
      flagged.push({ ...entry, reason: 'flagged by safe-regex (potential catastrophic backtracking)' });
    }
  }

  if (flagged.length > 0) {
    console.error(`FAIL: ${flagged.length} of ${patterns.length} regex patterns flagged as potentially vulnerable to ReDoS:`);
    for (const f of flagged) {
      console.error(`  - ${f.path}: ${f.reason}`);
      console.error(`      pattern: ${f.pattern}`);
    }
    console.error('');
    console.error('See grammars/textmate/README.md "ReDoS Discipline" for authoring rules.');
    process.exit(1);
  }

  console.log(`OK: ${patterns.length} regex patterns in ${path.basename(grammarPath)} all pass safe-regex lint`);
  process.exit(0);
}

main();
