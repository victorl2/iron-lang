// #312: the `iron` debug type. Starts a session on
// tests/integration/debug/stepping.iron with a breakpoint on line 3 and
// checks, through the adapter (`iron dap`), that it stops in area(w, h)
// called from main and that the locals show under their Iron names
// without compiler temporaries. Skipped when `iron` is not on PATH (or
// IRON_PATH), when no DAP debugger is installed, and on Windows (where
// the type hands off to the C/C++ extension).
import * as assert from 'node:assert';
import * as path from 'node:path';
import { spawnSync } from 'node:child_process';
import * as vscode from 'vscode';

function findIron(): string | undefined {
  if (process.env.IRON_PATH) return process.env.IRON_PATH;
  const r = spawnSync(process.platform === 'win32' ? 'where' : 'which', ['iron'], { encoding: 'utf8' });
  return r.status === 0 ? r.stdout.trim().split(/\r?\n/)[0] : undefined;
}

suite('iron-lsp e2e: iron debug type', () => {
  test('stops on a .iron breakpoint with Iron names', async function () {
    this.timeout(180_000);
    const iron = findIron();
    if (!iron || process.platform === 'win32') this.skip();
    await vscode.workspace.getConfiguration('iron.debug').update('ironPath', iron, vscode.ConfigurationTarget.Global);

    const repo = path.resolve(__dirname, '..', '..', '..', '..', '..');
    const src = path.join(repo, 'tests', 'integration', 'debug', 'stepping.iron');
    const uri = vscode.Uri.file(src);
    vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(uri, new vscode.Position(2, 0)))]);

    let stopped: (() => void) | undefined;
    let failed: ((e: Error) => void) | undefined;
    const stop = new Promise<void>((resolve, reject) => { stopped = resolve; failed = reject; });
    const tracker = vscode.debug.registerDebugAdapterTrackerFactory('iron', {
      createDebugAdapterTracker: () => ({
        onDidSendMessage: (m: any) => {
          if (m.type === 'event' && m.event === 'stopped') stopped?.();
          if (m.type === 'response' && !m.success && (m.command === 'initialize' || m.command === 'launch')) {
            failed?.(new Error(m.message ?? 'launch failed'));
          }
        },
      }),
    });
    try {
      const ok = await vscode.debug.startDebugging(undefined, {
        type: 'iron', request: 'launch', name: 'e2e', program: src,
      });
      assert.ok(ok, 'startDebugging returned false');
      try {
        await Promise.race([stop, new Promise((_, rej) => setTimeout(() => rej(new Error('no stop within 120s')), 120_000))]);
      } catch (e) {
        if (String(e).includes('no debugger found')) this.skip();
        throw e;
      }
      const session = vscode.debug.activeDebugSession!;
      const threads = await session.customRequest('threads');
      const tid = threads.threads[0].id;
      const st = await session.customRequest('stackTrace', { threadId: tid });
      const names: string[] = st.stackFrames.map((f: any) => f.name);
      assert.ok(names[0].startsWith('area'), `top frame is ${names[0]}`);
      const scopes = await session.customRequest('scopes', { frameId: st.stackFrames[0].id });
      const locals: Record<string, string> = {};
      for (const s of scopes.scopes) {
        if (!/^(locals|arguments)$/i.test(s.name)) continue;
        const vars = await session.customRequest('variables', { variablesReference: s.variablesReference });
        for (const v of vars.variables) locals[v.name] = v.value;
      }
      assert.strictEqual(locals.w, '0');
      assert.strictEqual(locals.h, '2');
      assert.ok(!Object.keys(locals).some((n) => n.startsWith('_')), `temporaries shown: ${Object.keys(locals)}`);
      await vscode.debug.stopDebugging(session);
    } finally {
      tracker.dispose();
      vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
    }
  });
});
