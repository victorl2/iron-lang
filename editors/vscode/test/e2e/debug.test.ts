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
import { terminalCommand } from '../../src/debug';
import { panicFrameIndex } from '../../src/panicFocus';

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

  test('test blocks get Run Test / Debug Test; .iron files get Run / Debug', async function () {
    this.timeout(60_000);
    const repo = path.resolve(__dirname, '..', '..', '..', '..', '..');
    const uri = vscode.Uri.file(path.join(repo, 'tests', 'integration', 'debug', 'conditions.iron'));
    await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri));
    const lenses = (await vscode.commands.executeCommand<vscode.CodeLens[]>(
      'vscode.executeCodeLensProvider', uri)) ?? [];
    const tests = lenses.filter((l) => l.command?.command === 'iron.runTest' ||
                                       l.command?.command === 'iron.debugTest');
    assert.strictEqual(tests.length, 2, `lenses: ${lenses.map((l) => l.command?.title)}`);
    for (const l of tests) {
      assert.strictEqual(l.range.start.line, 23);           // test "sums the first ten" {
      assert.deepStrictEqual(l.command?.arguments?.[1], 'sums the first ten');
    }
    const commands = await vscode.commands.getCommands(true);
    for (const c of ['iron.runFile', 'iron.debugFile', 'iron.runTest', 'iron.debugTest']) {
      assert.ok(commands.includes(c), `${c} is not registered`);
    }
  });

  // Run / Run Test type the command into the terminal's shell. On Windows
  // that is PowerShell by default, where a quoted program needs `&`: the
  // POSIX quoting failed there with "Unexpected token 'test'".
  test('Run commands are quoted for the terminal shell', () => {
    const argv = ['C:\\Program Files\\Iron\\iron.exe', 'test', 'C:\\work\\main.iron', "Bob's \"area\""];
    assert.strictEqual(terminalCommand('C:\\WINDOWS\\System32\\WindowsPowerShell\\v1.0\\powershell.exe', argv),
      "& 'C:\\Program Files\\Iron\\iron.exe' 'test' 'C:\\work\\main.iron' 'Bob''s \"area\"'");
    assert.strictEqual(terminalCommand('C:\\Program Files\\PowerShell\\7\\pwsh.exe', ['iron', 'run', 'a.iron']),
      "& 'iron' 'run' 'a.iron'");
    assert.strictEqual(terminalCommand('C:\\WINDOWS\\System32\\cmd.exe', argv),
      '"C:\\Program Files\\Iron\\iron.exe" test C:\\work\\main.iron "Bob\'s ""area"""');
    assert.strictEqual(terminalCommand('/bin/zsh', ['/usr/local/bin/iron', 'test', '/p/a.iron', 'area of a "square"']),
      '/usr/local/bin/iron test /p/a.iron "area of a \\"square\\""');
    assert.strictEqual(terminalCommand('C:\\Program Files\\Git\\bin\\bash.exe', ['iron', 'run', 'a b.iron']),
      'iron run "a b.iron"');
  });

  // #388: under the Visual Studio debugger (Windows) a panic that reaches
  // abort() focuses the first Iron frame; a stop already on an Iron line
  // (a --debug check's trap) or outside the panic path is left alone.
  test('a panic in abort() focuses the Iron frame', () => {
    const src = (p: string) => ({ path: p });
    const abort = [
      { id: 1, name: 'main_test.exe!abort() Line 77', source: src('minkernel\\crts\\ucrt\\src\\appcrt\\startup\\abort.cpp') },
      { id: 2, name: 'main_test.exe!Iron_read_file(Iron_String path) Line 71', source: src('C:\\iron\\lib\\runtime\\iron_builtins.c') },
      { id: 3, name: 'main_test.exe!Iron_load(Iron_String path) Line 2', source: src('C:\\work\\main.iron') },
      { id: 4, name: 'main_test.exe!Iron_main() Line 6', source: src('C:\\work\\main.iron') },
    ];
    assert.strictEqual(panicFrameIndex(abort), 2);
    assert.strictEqual(panicFrameIndex(abort.slice(2)), -1);           // the trap: already Iron
    const crash = [
      { id: 1, name: 'main.exe!memcpy() Line 10', source: src('memcpy.asm') },
      { id: 2, name: 'main.exe!Iron_main() Line 6', source: src('C:\\work\\main.iron') },
    ];
    assert.strictEqual(panicFrameIndex(crash), -1);                    // not a panic
    assert.strictEqual(panicFrameIndex([abort[0], abort[1]]), -1);     // no Iron frame
  });

  test('Debug Test stops in the test block', async function () {
    this.timeout(180_000);
    const iron = findIron();
    if (!iron || process.platform === 'win32') this.skip();
    await vscode.workspace.getConfiguration('iron.debug').update('ironPath', iron, vscode.ConfigurationTarget.Global);
    const repo = path.resolve(__dirname, '..', '..', '..', '..', '..');
    const uri = vscode.Uri.file(path.join(repo, 'tests', 'integration', 'debug', 'conditions.iron'));
    vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(uri, new vscode.Position(28, 0)))]);
    let stopped: ((tid: number) => void) | undefined;
    const stop = new Promise<number>((resolve) => { stopped = resolve; });
    const tracker = vscode.debug.registerDebugAdapterTrackerFactory('iron', {
      createDebugAdapterTracker: () => ({
        onDidSendMessage: (m: any) => {
          if (m.type === 'event' && m.event === 'stopped') stopped?.(m.body.threadId);
        },
      }),
    });
    try {
      await vscode.commands.executeCommand('iron.debugTest', uri, 'sums the first ten');
      const tid = await stop;
      const session = vscode.debug.activeDebugSession!;
      const st = await session.customRequest('stackTrace', { threadId: tid });
      assert.strictEqual(st.stackFrames[0].line, 29);
      const total = await session.customRequest('evaluate',
        { expression: 'total', frameId: st.stackFrames[0].id, context: 'watch' });
      assert.strictEqual(total.result, '45');
      await vscode.debug.stopDebugging(session);
    } finally {
      tracker.dispose();
      vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
    }
  });
});
