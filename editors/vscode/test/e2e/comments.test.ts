// Iron's line comment is `//` (manual 1.2: one comment form, no block
// comments). Toggle Line Comment must write `//`.
import * as assert from 'node:assert';
import * as vscode from 'vscode';

suite('iron-lsp e2e: language configuration', () => {
  test('Toggle Line Comment writes //', async function () {
    this.timeout(30_000);
    const doc = await vscode.workspace.openTextDocument({ language: 'iron', content: 'val x = 1\n' });
    const editor = await vscode.window.showTextDocument(doc);
    editor.selection = new vscode.Selection(0, 0, 0, 0);
    await vscode.commands.executeCommand('editor.action.commentLine');
    assert.strictEqual(doc.lineAt(0).text, '// val x = 1');
    await vscode.commands.executeCommand('editor.action.commentLine');
    assert.strictEqual(doc.lineAt(0).text, 'val x = 1');
    await vscode.commands.executeCommand('workbench.action.revertAndCloseActiveEditor');
  });
});
