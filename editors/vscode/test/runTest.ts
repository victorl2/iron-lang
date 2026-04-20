// Phase 6 Plan 06-03 Task 3 (EXT-04, @vscode/test-electron harness).
// Spawns a headless VSCode Electron instance and runs the e2e tests
// under test/e2e/. On macOS arm64 the GPU sandbox is disabled per
// RESEARCH Pitfall 7 so the test runner does not hang on the launch
// screen.
import * as path from 'node:path';
import { runTests } from '@vscode/test-electron';

async function main(): Promise<void> {
  // __dirname at runtime = editors/vscode/out/test
  // extensionDevelopmentPath = editors/vscode (contains package.json + dist/)
  const extensionDevelopmentPath = path.resolve(__dirname, '..', '..');
  // extensionTestsPath = editors/vscode/out/test/e2e/index.js (compiled by tsc)
  const extensionTestsPath = path.resolve(__dirname, './e2e/index');

  const launchArgs: string[] = ['--disable-extensions'];
  if (process.platform === 'darwin' && process.arch === 'arm64') {
    // PITFALLS §7: macOS arm64 runners otherwise hang on GPU init.
    launchArgs.push('--disable-gpu-sandbox');
  }

  await runTests({
    extensionDevelopmentPath,
    extensionTestsPath,
    launchArgs,
  });
}

main().catch((err) => {
  // eslint-disable-next-line no-console
  console.error('vscode-e2e runTest failed:', err);
  process.exit(1);
});
