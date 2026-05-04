#!/usr/bin/env node
// Phase 6 Plan 06-01 Task 2 (EXT-01, CONTEXT D-02). Runs
// vscode-tmgrammar-test over the fixture corpus in ./fixtures/ and
// asserts scope snapshots match.
//
// The binary ships at node_modules/.bin/vscode-tmgrammar-test; exit 2
// if the dep is missing (prompts the CI fixture `test_grammars_npm_install`
// to run), exit 1 on fixture/grammar mismatch, exit 0 when green.
//
// Invoked by CTest test_grammar_textmate_snapshot (phase-m5-invariant).

'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const TESTS_DIR = __dirname;
const REPO_ROOT = path.resolve(TESTS_DIR, '..', '..', '..');
const GRAMMAR_PATH = path.join(REPO_ROOT,
  'grammars', 'textmate', 'iron.tmLanguage.json');
const FIXTURE_GLOB = path.join(TESTS_DIR, 'fixtures', '*.iron');

function resolveCli() {
  const localBin = path.join(TESTS_DIR, 'node_modules', '.bin',
    'vscode-tmgrammar-test');
  if (fs.existsSync(localBin)) return localBin;
  return null;
}

function main() {
  if (!fs.existsSync(GRAMMAR_PATH)) {
    console.error(`error: grammar missing at ${GRAMMAR_PATH}`);
    process.exit(2);
  }

  const cli = resolveCli();
  if (!cli) {
    console.error('SKIPPED: vscode-tmgrammar-test not installed. Run');
    console.error('         `npm install --prefix ' + TESTS_DIR + '` first.');
    console.error('         (CTest fixture `test_grammars_npm_install` performs this step.)');
    // Exit 77 is CTest's "skip" convention when SKIP_RETURN_CODE is set.
    process.exit(77);
  }

  // vscode-tmgrammar-test CLI: `vscode-tmgrammar-test -g <grammar> <glob>`
  // on success prints a pass banner and exits 0; on failure prints a
  // diff and exits non-zero.
  const res = spawnSync(cli, ['-g', GRAMMAR_PATH, FIXTURE_GLOB], {
    stdio: 'inherit',
    cwd: TESTS_DIR,
  });

  if (res.error) {
    console.error(`error: failed to spawn ${cli}: ${res.error.message}`);
    process.exit(2);
  }

  process.exit(res.status ?? 1);
}

main();
