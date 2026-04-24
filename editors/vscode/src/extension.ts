// Phase 6 Plan 06-03 Task 2 (EXT-04, CONTEXT D-04, D-15, UI-SPEC S1/S2/S5/S9):
// activate/deactivate entry points for the Iron LSP VSCode extension.
//
// Flow:
//   1. Create "Iron Language Server" OutputChannel (UI-SPEC S2).
//   2. Discover ironls binary via setting -> PATH (server.ts); early-exit
//      on failure (error toast already shown).
//   3. Probe `ironls --version`; HARD-REFUSE activation if outside
//      ironLspCompatibleIronlsRange (Phase 7 Plan 07-07 HARD-22 / D-10;
//      tightens Phase 6 D-11 warn-only posture).
//   4. Lazily create "Iron Language Server (trace)" when
//      iron.languageServer.trace.server != off (UI-SPEC S2).
//   5. Construct + start LanguageClient over stdio with
//      synchronize.fileEvents on **/iron.toml + **/iron.lock.
//   6. Register 500 ms debounced restart on
//      iron.languageServer.* config changes (PITFALLS §2).
//   7. Register iron-lsp.diagnose command (UI-SPEC S3).
//
// deactivate() awaits client.stop() per PITFALLS §11 so VSCode exits
// cleanly without orphan ironls processes.
//
// Phase 7 Plan 07-07 Task 02 (HARD-22, D-10, UI-SPEC S9):
// Version mismatch is a HARD REFUSE — we show an error with an
// "Update Iron LSP" button, do NOT start the LanguageClient, and
// DO NOT register any language features. A missing serverInfo.version
// (pre-stamped server built before HARD-22) also triggers the refuse
// path; users running a pre-release server must upgrade the binary.

import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';
import { discoverIronls, probeIronlsVersion } from './server';
import { runDiagnose } from './diagnose';
import { logEvent } from './log';

let client: LanguageClient | undefined;
let output: vscode.OutputChannel | undefined;
let traceChannel: vscode.OutputChannel | undefined;

export async function activate(
  context: vscode.ExtensionContext
): Promise<void> {
  output = vscode.window.createOutputChannel('Iron Language Server');
  context.subscriptions.push(output);

  logEvent(output, 'info', 'ext.activate', {
    editor_version: vscode.version,
    platform: `${process.platform}-${process.arch}`,
  });

  const serverPath = await discoverIronls(output);
  if (!serverPath) {
    // Error toast already shown by discoverIronls; extension remains
    // active so the user can retry after fixing the setting — the
    // onDidChangeConfiguration listener below is not yet registered, so
    // the user would reload the window after setting the path.
    return;
  }

  const { version: serverVersion, raw: versionRaw } =
    probeIronlsVersion(serverPath);
  const compatibleRange: string =
    (context.extension.packageJSON as Record<string, unknown>)[
      'ironLspCompatibleIronlsRange'
    ] as string | undefined ?? '';

  // Phase 7 Plan 07-07 (HARD-22, D-10, UI-SPEC S9): HARD REFUSE on
  // version mismatch. We refuse BEFORE starting the LanguageClient so no
  // handlers, trace channel, or config watcher activate for an
  // incompatible server. Phase 6 D-11 warned here; Phase 7 tightens.
  if (serverVersion) {
    const compatible = isCompatible(serverVersion, compatibleRange);
    logEvent(output, 'info', 'ironls.version_check', {
      version: serverVersion,
      compatible,
      range: compatibleRange,
    });
    if (!compatible) {
      const [min, max] = parseRangeForMessage(compatibleRange);
      logEvent(output, 'error', 'ironls.version_mismatch', {
        version: serverVersion,
        range: compatibleRange,
        action: 'refuse-activation',
      });
      // UI-SPEC S9: detected version + compatible range + install URL +
      // clickable upgrade action. showErrorMessage is the hard surface
      // (red toast with Modal-style emphasis in VSCode's UI).
      void vscode.window
        .showErrorMessage(
          `Iron LSP: detected ironls ${serverVersion}, but this extension requires ${min} .. ${max}. The language server will NOT activate. Install the latest ironls to continue.`,
          'Update Iron LSP'
        )
        .then((sel) => {
          if (sel === 'Update Iron LSP') {
            void vscode.env.openExternal(
              vscode.Uri.parse(
                'https://github.com/iron-lang/iron-lang/releases/latest'
              )
            );
          }
        });
      return;
    }
  } else {
    logEvent(output, 'error', 'ironls.version_mismatch', {
      version: null,
      raw: versionRaw,
      range: compatibleRange,
      action: 'refuse-activation',
    });
    // Pre-HARD-22 binaries or binaries that fail the --version probe —
    // refuse with an actionable upgrade path. This is the structural
    // guarantee that an unstamped binary can't slip past the contract.
    void vscode.window
      .showErrorMessage(
        `Iron LSP: could not determine the ironls version (expected ${compatibleRange}). The language server will NOT activate. Install the latest ironls to continue.`,
        'Update Iron LSP'
      )
      .then((sel) => {
        if (sel === 'Update Iron LSP') {
          void vscode.env.openExternal(
            vscode.Uri.parse(
              'https://github.com/iron-lang/iron-lang/releases/latest'
            )
          );
        }
      });
    return;
  }

  const serverOptions: ServerOptions = {
    command: serverPath,
    args: [],
    transport: TransportKind.stdio,
    options: { env: process.env },
  };

  // UI-SPEC S2: lazy trace channel.
  const traceSetting =
    vscode.workspace
      .getConfiguration('iron.languageServer')
      .get<string>('trace.server') ?? 'off';
  if (traceSetting !== 'off' && !traceChannel) {
    traceChannel = vscode.window.createOutputChannel(
      'Iron Language Server (trace)'
    );
    context.subscriptions.push(traceChannel);
  }

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'iron' }],
    synchronize: {
      fileEvents: [
        vscode.workspace.createFileSystemWatcher('**/iron.toml'),
        vscode.workspace.createFileSystemWatcher('**/iron.lock'),
      ],
    },
    outputChannel: output,
    traceOutputChannel: traceChannel,
    initializationOptions: { clientName: 'vscode' },
  };

  client = new LanguageClient(
    'iron-lsp',
    'Iron Language Server',
    serverOptions,
    clientOptions
  );

  logEvent(output, 'info', 'lsp.initialize.start', { path: serverPath });
  try {
    await client.start();
    logEvent(output, 'info', 'ironls.spawn.ok');
    logEvent(output, 'info', 'lsp.initialize.ok', {
      server_version: serverVersion ?? 'unknown',
    });
  } catch (e) {
    const reason = e instanceof Error ? e.message : String(e);
    logEvent(output, 'error', 'lsp.initialize.failed', { reason });
    void vscode.window.showErrorMessage(
      'Iron LSP: initialization failed. See the "Iron Language Server" output channel.'
    );
    return;
  }

  // PITFALLS §2: debounced restart guarded by affectsConfiguration.
  let restartTimer: NodeJS.Timeout | undefined;
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (!e.affectsConfiguration('iron.languageServer')) return;
      if (restartTimer) clearTimeout(restartTimer);
      restartTimer = setTimeout(async () => {
        if (!client || !output) return;
        logEvent(output, 'info', 'lsp.shutdown', {
          reason: 'config-change-restart',
        });
        try {
          await client.stop();
          await client.start();
          logEvent(output, 'info', 'lsp.initialize.ok', {
            reason: 'config-change-restart',
          });
        } catch (err) {
          const reason = err instanceof Error ? err.message : String(err);
          logEvent(output, 'error', 'ironls.spawn.failed', { reason });
        }
      }, 500);
    })
  );

  // UI-SPEC S3: diagnose command.
  context.subscriptions.push(
    vscode.commands.registerCommand('iron-lsp.diagnose', () =>
      runDiagnose({
        extensionVersion:
          (context.extension.packageJSON as Record<string, unknown>)[
            'version'
          ] as string ?? '0.0.0',
        ironlsPath: serverPath,
        ironlsVersion: serverVersion,
        compatibleRange,
        client,
        output: output!,
      })
    )
  );
}

export async function deactivate(): Promise<void> {
  if (client) {
    if (output) {
      logEvent(output, 'info', 'lsp.shutdown', { reason: 'deactivate' });
    }
    // PITFALLS §11: MUST await so the ironls child exits cleanly.
    await client.stop();
    client = undefined;
  }
}

// ---------- minimal semver helpers for ironLspCompatibleIronlsRange ----------
// Range format parsed: ">=X.Y.Z, <A.B.C" (the shape used in package.json).
// Anything else is treated as permissive (return true) to avoid false
// warnings for unrecognized range syntax.

function isCompatible(version: string, range: string): boolean {
  if (!range) return true;
  const m = range.match(
    />=?\s*(\d+\.\d+\.\d+[^\s,]*)[\s,]+<\s*(\d+\.\d+\.\d+[^\s]*)/
  );
  if (!m) return true;
  const [, min, max] = m;
  return semverGte(version, min) && semverLt(version, max);
}

function parseRangeForMessage(range: string): [string, string] {
  const m = range.match(/>=?\s*([^\s,]+)[\s,]+<\s*([^\s]+)/);
  return m ? [m[1], m[2]] : ['?', '?'];
}

function semverKey(v: string): [number, number, number] {
  const m = v.match(/^(\d+)\.(\d+)\.(\d+)/);
  return m ? [+m[1], +m[2], +m[3]] : [0, 0, 0];
}

function semverGte(a: string, b: string): boolean {
  const ka = semverKey(a);
  const kb = semverKey(b);
  for (let i = 0; i < 3; i++) {
    if (ka[i] !== kb[i]) return ka[i] > kb[i];
  }
  return true;
}

function semverLt(a: string, b: string): boolean {
  const ka = semverKey(a);
  const kb = semverKey(b);
  for (let i = 0; i < 3; i++) {
    if (ka[i] !== kb[i]) return ka[i] < kb[i];
  }
  return false;
}
