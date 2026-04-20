// Phase 6 Plan 06-03 Task 3: Mocha entry point consumed by
// @vscode/test-electron. Collects every `*.test.js` under this directory
// (already compiled by `tsc -p . --outDir out`) and runs them via Mocha's
// TDD UI. Failure count bubbles up as a rejected promise so the outer
// runTests() call fails the CI job.
import * as path from 'node:path';
import Mocha from 'mocha';
import { glob } from 'glob';

export async function run(): Promise<void> {
  const mocha = new Mocha({
    ui: 'tdd',
    color: true,
    timeout: 30_000,
  });

  const testsRoot = path.resolve(__dirname, '.');
  const files = await glob('**/*.test.js', { cwd: testsRoot });

  for (const f of files) {
    mocha.addFile(path.resolve(testsRoot, f));
  }

  await new Promise<void>((resolve, reject) => {
    mocha.run((failures) => {
      if (failures > 0) {
        reject(new Error(`${failures} test(s) failed`));
      } else {
        resolve();
      }
    });
  });
}
