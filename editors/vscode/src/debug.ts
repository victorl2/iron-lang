// The `iron` debug type (#312): F5 on a .iron file, or a launch
// configuration of type "iron", debugs Iron programs at the Iron level.
//
// On Linux and macOS the debug adapter is `iron dap` (lib/debug/iron_dap.py
// in the Iron installation), which also serves Neovim and Zed: it builds
// the program with --debug, runs lldb-dap or gdb, loads the value
// formatters, shows locals under their Iron names and stops on panics.
//
// On Windows there is no lldb-dap in the toolchain; the configuration is
// handed to the C/C++ extension's Visual Studio debugger (cppvsdbg),
// which reads the PDB and the natvis that --debug links into it. A panic
// there stops on the Iron line: through the debug trap of a --debug build's
// checks, and through panicFocus.ts for the rest.

import * as vscode from 'vscode';
import { spawn, spawnSync } from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { IRON_SESSION, registerPanicFocus } from './panicFocus';

const TYPE = 'iron';

export function registerDebugger(context: vscode.ExtensionContext, output: vscode.OutputChannel): void {
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider(TYPE, new IronConfigurationProvider(output)),
    vscode.debug.registerDebugConfigurationProvider(TYPE, {
      provideDebugConfigurations: () => [defaultConfiguration()],
    }, vscode.DebugConfigurationProviderTriggerKind.Dynamic),
    vscode.debug.registerDebugAdapterDescriptorFactory(TYPE, {
      createDebugAdapterDescriptor: () => new vscode.DebugAdapterExecutable(ironCli(), ['dap']),
    }),
    // Run / Debug in the editor title of a .iron file, and on each test block.
    vscode.commands.registerCommand('iron.runFile', (uri?: vscode.Uri) => {
      const file = targetFile(uri);
      if (file) runInTerminal(['run', file], path.dirname(file));
    }),
    vscode.commands.registerCommand('iron.debugFile', (uri?: vscode.Uri) => {
      const file = targetFile(uri);
      if (file) void vscode.debug.startDebugging(vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file)),
        { type: TYPE, request: 'launch', name: `Iron: debug ${path.basename(file)}`, program: file });
    }),
    vscode.commands.registerCommand('iron.runTest', (uri: vscode.Uri, name: string) => {
      runInTerminal(['test', uri.fsPath, name], path.dirname(uri.fsPath));
    }),
    vscode.commands.registerCommand('iron.debugTest', (uri: vscode.Uri, name: string) => {
      void vscode.debug.startDebugging(vscode.workspace.getWorkspaceFolder(uri),
        { type: TYPE, request: 'launch', name: `Iron: debug test "${name}"`, program: uri.fsPath, test: name });
    }),
    vscode.languages.registerCodeLensProvider({ language: 'iron' }, new TestLensProvider()),
  );
  registerPanicFocus(context);
}

/** The .iron file a command acts on: the one clicked, else the active editor's. */
function targetFile(uri?: vscode.Uri): string | undefined {
  const doc = uri ? undefined : vscode.window.activeTextEditor?.document;
  const file = uri?.fsPath ?? (doc?.languageId === 'iron' ? doc.uri.fsPath : undefined);
  if (!file) void vscode.window.showErrorMessage('Iron: open a .iron file first.');
  return file;
}

/** Run the iron CLI in a terminal of its own (reused between runs). */
function runInTerminal(args: string[], cwd: string): void {
  const term = vscode.window.terminals.find((t) => t.name === 'Iron') ??
    vscode.window.createTerminal({ name: 'Iron', cwd });
  term.show(true);
  term.sendText(terminalCommand(vscode.env.shell, [ironCli(), ...args]));
}

/** The command line that runs `argv` in `shell` (the terminal's default
 * shell): PowerShell needs the call operator before a quoted program and
 * takes single quotes, cmd takes double quotes without backslash escapes,
 * and POSIX shells (also Git Bash on Windows) take escaped double quotes. */
export function terminalCommand(shell: string, argv: string[]): string {
  const name = path.win32.basename(shell).toLowerCase();
  const plain = (a: string) => /^[\w./:=-]+$/.test(a);
  if (/^(pwsh|powershell)(\.exe)?$/.test(name)) {
    const q = (a: string) => `'${a.replace(/'/g, "''")}'`;
    return ['&', ...argv.map(q)].join(' ');
  }
  if (name === 'cmd.exe' || name === 'cmd') {
    return argv.map((a) => (/^[\w.:\\=-]+$/.test(a) ? a : `"${a.replace(/"/g, '""')}"`)).join(' ');
  }
  return argv.map((a) => (plain(a) ? a : `"${a.replace(/(["\\$`])/g, '\\$1')}"`)).join(' ');
}

/** "Run Test | Debug Test" above every `test "name" {` line. */
export class TestLensProvider implements vscode.CodeLensProvider {
  provideCodeLenses(doc: vscode.TextDocument): vscode.CodeLens[] {
    const lenses: vscode.CodeLens[] = [];
    for (let i = 0; i < doc.lineCount; i++) {
      const m = /^\s*test\s+"((?:[^"\\]|\\.)*)"\s*\{/.exec(doc.lineAt(i).text);
      if (!m) continue;
      const name = m[1].replace(/\\(.)/g, '$1');
      const range = new vscode.Range(i, 0, i, 0);
      lenses.push(
        new vscode.CodeLens(range, { title: '$(play) Run Test', command: 'iron.runTest', arguments: [doc.uri, name] }),
        new vscode.CodeLens(range, { title: '$(debug-alt) Debug Test', command: 'iron.debugTest', arguments: [doc.uri, name] }),
      );
    }
    return lenses;
  }
}

export function defaultConfiguration(): vscode.DebugConfiguration {
  return { type: TYPE, request: 'launch', name: 'Iron: debug this file', program: '${file}' };
}

/** The `iron` CLI: the iron.debug.ironPath setting, else PATH. */
function ironCli(): string {
  const configured = (vscode.workspace.getConfiguration('iron.debug').get<string>('ironPath') ?? '').trim();
  if (configured) return configured;
  const probe = process.platform === 'win32' ? 'where' : 'which';
  const r = spawnSync(probe, ['iron'], { encoding: 'utf8' });
  if (r.status === 0 && r.stdout) return r.stdout.trim().split(/\r?\n/)[0];
  return 'iron';
}

class IronConfigurationProvider implements vscode.DebugConfigurationProvider {
  constructor(private output: vscode.OutputChannel) {}

  resolveDebugConfiguration(folder: vscode.WorkspaceFolder | undefined, config: vscode.DebugConfiguration):
      vscode.DebugConfiguration | undefined {
    // F5 without a launch.json: debug the open .iron file.
    if (!config.type && !config.request && !config.name) {
      if (vscode.window.activeTextEditor?.document.languageId !== 'iron') return config;
      return defaultConfiguration();
    }
    if (!config.program) config.program = folder?.uri.fsPath ?? '${file}';
    return config;
  }

  async resolveDebugConfigurationWithSubstitutedVariables(
      folder: vscode.WorkspaceFolder | undefined, config: vscode.DebugConfiguration):
      Promise<vscode.DebugConfiguration | undefined> {
    if (process.platform !== 'win32') return config;  // iron dap builds and launches
    // Windows: build here, then debug with the Visual Studio debugger.
    const binary = await buildForWindows(config.program, this.output, Boolean(config.test));
    if (!binary) return undefined;
    let args: string[] = config.args ?? [];
    if (config.test) {
      // A test binary lists its tests with --iron-list and runs test n with --iron-test n.
      const list = spawnSync(binary, ['--iron-list'], { encoding: 'utf8' });
      const names = list.status === 0 ? list.stdout.split(/\r?\n/).filter((l) => l) : [];
      const index = names.indexOf(config.test);
      if (index < 0) {
        void vscode.window.showErrorMessage(`Iron: no test "${config.test}" in ${path.basename(config.program)}.`);
        return undefined;
      }
      args = ['--iron-test', String(index), ...args];
    }
    if (!vscode.extensions.getExtension('ms-vscode.cpptools')) {
      void vscode.window.showErrorMessage(
        'Iron: debugging on Windows uses the C/C++ extension (ms-vscode.cpptools); install it and try again.');
      return undefined;
    }
    const program: string = config.program;
    await vscode.debug.startDebugging(folder, {
      type: 'cppvsdbg', request: 'launch', name: config.name, program: binary,
      args,
      cwd: config.cwd ?? (program.endsWith('.iron') ? path.dirname(program) : program),
      environment: Object.entries(config.env ?? {}).map(([name, value]) => ({ name, value })),
      console: 'integratedTerminal',
      [IRON_SESSION]: true,  // a panic focuses the Iron frame (panicFocus.ts)
    });
    return undefined;  // the cppvsdbg session replaces this one
  }
}

/** Windows: build `program` (a .iron file or a package directory) with
 * --debug and return the .exe, or undefined when the build failed. */
async function buildForWindows(program: string, output: vscode.OutputChannel, test = false): Promise<string | undefined> {
  const iron = ironCli();
  let cwd: string, args: string[], binary: string;
  if (program.endsWith('.iron')) {
    const dir = path.join(os.tmpdir(), 'iron-debug');
    fs.mkdirSync(dir, { recursive: true });
    binary = path.join(dir, path.basename(program, '.iron') + (test ? '_test' : '') + '.exe');
    cwd = path.dirname(program);
    args = ['build', program, '--debug', ...(test ? ['--test'] : []), '-o', binary];
  } else if (test) {
    void vscode.window.showErrorMessage('Iron: to debug a test, debug the .iron file that declares it.');
    return undefined;
  } else {
    let dir = program;
    while (!fs.existsSync(path.join(dir, 'iron.toml')) && path.dirname(dir) !== dir) dir = path.dirname(dir);
    const toml = path.join(dir, 'iron.toml');
    const name = fs.existsSync(toml) ? /^\s*name\s*=\s*"([^"]+)"/m.exec(fs.readFileSync(toml, 'utf8'))?.[1] : undefined;
    if (!name) {
      void vscode.window.showErrorMessage(`Iron: no iron.toml found for "${program}".`);
      return undefined;
    }
    cwd = dir;
    args = ['build', '--debug'];
    binary = path.join(dir, 'target', name + '.exe');
  }
  output.appendLine(`$ ${iron} ${args.join(' ')}`);
  const code = await new Promise<number>((resolve) => {
    const p = spawn(iron, args, { cwd });
    p.stdout.on('data', (d) => output.append(d.toString()));
    p.stderr.on('data', (d) => output.append(d.toString()));
    p.on('error', (e) => { output.appendLine(`cannot run ${iron}: ${e.message}`); resolve(-1); });
    p.on('exit', (c) => resolve(c ?? -1));
  });
  if (code !== 0) {
    output.show(true);
    void vscode.window.showErrorMessage('Iron: the --debug build failed; see the Iron Language Server output.');
    return undefined;
  }
  return binary;
}
