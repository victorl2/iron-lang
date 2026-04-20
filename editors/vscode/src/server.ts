// Phase 6 Plan 06-03 Task 2 (EXT-04, UI-SPEC S1, CONTEXT D-04): binary
// discovery + --version probe for ironls. Threat model T-06-03-01: every
// spawn uses argv-form (no shell interpolation) and executability is
// checked via fs.accessSync(X_OK) before launch.

import * as vscode from 'vscode';
import { spawnSync } from 'node:child_process';
import * as fs from 'node:fs';
import { logEvent } from './log';

/**
 * Resolve the ironls executable per UI-SPEC S1 / CONTEXT D-04:
 *   1. iron.languageServer.path setting — if set, it MUST point at an
 *      executable file. If not, show S1 error #2 with an "Open Settings"
 *      button and return undefined.
 *   2. Otherwise probe PATH via `which`/`where`. If found, return the
 *      first match.
 *   3. Otherwise show S1 error #1 with an "Open Settings" button and
 *      return undefined.
 *
 * All error paths surface a toast with an actionable next step — never
 * silently fail (UI-SPEC S1 requirement).
 */
export async function discoverIronls(
  output: vscode.OutputChannel
): Promise<string | undefined> {
  const cfg = vscode.workspace.getConfiguration('iron.languageServer');
  const configured = (cfg.get<string>('path') ?? '').trim();

  if (configured.length > 0) {
    if (fs.existsSync(configured) && isExecutable(configured)) {
      logEvent(output, 'info', 'ironls.discovered', {
        path: configured,
        method: 'iron.languageServer.path setting',
      });
      return configured;
    }
    logEvent(output, 'error', 'ironls.not_found', {
      searched: [configured],
      reason: 'configured-path-not-executable',
    });
    // UI-SPEC S1 error #2 — verbatim text.
    const choice = await vscode.window.showErrorMessage(
      `Iron LSP: "iron.languageServer.path" points to "${configured}" which is not executable. Check the path in settings.`,
      'Open Settings'
    );
    if (choice === 'Open Settings') {
      await vscode.commands.executeCommand(
        'workbench.action.openSettings',
        'iron.languageServer'
      );
    }
    return undefined;
  }

  const probe = process.platform === 'win32' ? 'where' : 'which';
  // spawnSync argv form — no shell: true. Threat model T-06-03-01.
  const r = spawnSync(probe, ['ironls'], { encoding: 'utf8' });
  if (r.status === 0 && r.stdout) {
    const candidate = r.stdout.trim().split(/\r?\n/)[0];
    if (candidate && fs.existsSync(candidate) && isExecutable(candidate)) {
      logEvent(output, 'info', 'ironls.discovered', {
        path: candidate,
        method: 'PATH',
      });
      return candidate;
    }
  }

  logEvent(output, 'error', 'ironls.not_found', {
    searched: [`$PATH via ${probe}`],
    reason: 'not-on-path',
  });
  // UI-SPEC S1 error #1 — verbatim text.
  const choice = await vscode.window.showErrorMessage(
    'Iron LSP: could not find "ironls" on PATH. Install from https://iron-lang.dev/install or set "iron.languageServer.path" in settings.',
    'Open Settings'
  );
  if (choice === 'Open Settings') {
    await vscode.commands.executeCommand(
      'workbench.action.openSettings',
      'iron.languageServer'
    );
  }
  return undefined;
}

/**
 * Run `ironls --version` with a 3 s timeout and parse out the first
 * semver-shaped token from stdout. Returns { version: null } on failure.
 * Used by extension.ts for the UI-SPEC S9 compatibility-range check.
 */
export function probeIronlsVersion(path: string): {
  version: string | null;
  raw: string;
} {
  try {
    const r = spawnSync(path, ['--version'], {
      encoding: 'utf8',
      timeout: 3000,
    });
    if (r.status !== 0) {
      return { version: null, raw: (r.stderr || r.stdout || '').trim() };
    }
    const out = r.stdout ?? '';
    const m = out.match(/(\d+\.\d+\.\d+(?:-[a-zA-Z0-9.]+)?)/);
    return { version: m ? m[1] : null, raw: out.trim() };
  } catch {
    return { version: null, raw: '' };
  }
}

function isExecutable(p: string): boolean {
  try {
    fs.accessSync(p, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}
