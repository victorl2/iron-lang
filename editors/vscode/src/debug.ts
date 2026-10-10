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
// which reads the PDB and the natvis that --debug links into it.

import * as vscode from 'vscode';
import { spawn, spawnSync } from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';

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
  );
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
    const binary = await buildForWindows(config.program, this.output);
    if (!binary) return undefined;
    if (!vscode.extensions.getExtension('ms-vscode.cpptools')) {
      void vscode.window.showErrorMessage(
        'Iron: debugging on Windows uses the C/C++ extension (ms-vscode.cpptools); install it and try again.');
      return undefined;
    }
    const program: string = config.program;
    await vscode.debug.startDebugging(folder, {
      type: 'cppvsdbg', request: 'launch', name: config.name, program: binary,
      args: config.args ?? [],
      cwd: config.cwd ?? (program.endsWith('.iron') ? path.dirname(program) : program),
      environment: Object.entries(config.env ?? {}).map(([name, value]) => ({ name, value })),
      console: 'integratedTerminal',
    });
    return undefined;  // the cppvsdbg session replaces this one
  }
}

/** Windows: build `program` (a .iron file or a package directory) with
 * --debug and return the .exe, or undefined when the build failed. */
async function buildForWindows(program: string, output: vscode.OutputChannel): Promise<string | undefined> {
  const iron = ironCli();
  let cwd: string, args: string[], binary: string;
  if (program.endsWith('.iron')) {
    const dir = path.join(os.tmpdir(), 'iron-debug');
    fs.mkdirSync(dir, { recursive: true });
    binary = path.join(dir, path.basename(program, '.iron') + '.exe');
    cwd = path.dirname(program);
    args = ['build', program, '--debug', '-o', binary];
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
