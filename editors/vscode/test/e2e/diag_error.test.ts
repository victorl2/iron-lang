// Phase 6 Plan 06-03 Task 3 (EXT-04, EXT-10): end-to-end assertion that
// opening the shared `tests/editors/fixtures/diag_error.iron` fixture
// publishes at least one Error-severity diagnostic within 10 s. The
// fixture is authored to emit exactly one IRON_ERR_UNDEFINED_VAR (error
// code 200) via `val x: int = undefined_symbol` and is shared across all
// three editor e2e harnesses (Plans 06-03, 06-04, 06-05, 06-06).
import * as assert from 'node:assert';
import * as path from 'node:path';
import * as vscode from 'vscode';

suite('iron-lsp e2e: diag_error fixture', () => {
  test('publishes at least one Error-severity diagnostic within 10s', async () => {
    // __dirname at runtime = editors/vscode/out/test/e2e
    // Up 5 levels = repo root.
    const repo = path.resolve(
      __dirname,
      '..',
      '..',
      '..',
      '..',
      '..'
    );
    const fixture = path.join(
      repo,
      'tests',
      'editors',
      'fixtures',
      'diag_error.iron'
    );

    const doc = await vscode.workspace.openTextDocument(fixture);
    await vscode.window.showTextDocument(doc);

    const deadline = Date.now() + 10_000;
    let diags: readonly vscode.Diagnostic[] = [];
    while (Date.now() < deadline) {
      diags = vscode.languages.getDiagnostics(doc.uri);
      if (diags.length > 0) break;
      await new Promise((r) => setTimeout(r, 200));
    }

    assert.ok(
      diags.length > 0,
      `expected >= 1 diagnostic within 10s, got 0 (is ironls on PATH? IRONLS_PATH=${process.env.IRONLS_PATH ?? '<unset>'})`
    );
    assert.ok(
      diags.some((d) => d.severity === vscode.DiagnosticSeverity.Error),
      'expected at least one Error-severity diagnostic'
    );
  });
});
