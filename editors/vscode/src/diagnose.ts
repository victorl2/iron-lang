// Phase 6 Plan 06-03 Task 2 (EXT-04, UI-SPEC S3): iron-lsp.diagnose
// command. Produces the UI-SPEC S3 payload (order-locked 9-section
// report) and opens it in an untitled document for easy copy-paste into
// bug reports.
//
// Threat model T-06-03-02: payload excludes env vars, tokens, and user
// source by construction — only file paths, version strings, and LSP
// capability summaries.

import * as vscode from 'vscode';
import * as fs from 'node:fs';
import * as path from 'node:path';
import type { LanguageClient } from 'vscode-languageclient/node';

export interface DiagnoseArgs {
  extensionVersion: string;
  ironlsPath: string;
  ironlsVersion: string | null;
  compatibleRange: string;
  client: LanguageClient | undefined;
  output: vscode.OutputChannel;
}

export async function runDiagnose(a: DiagnoseArgs): Promise<void> {
  const now = new Date().toISOString();
  const folder =
    vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? 'NONE';
  const ironToml =
    folder !== 'NONE' ? pathIfExists(path.join(folder, 'iron.toml')) : 'NO';
  const ironLock =
    folder !== 'NONE' ? pathIfExists(path.join(folder, 'iron.lock')) : 'NO';

  let capsSummary: string;
  try {
    const caps = a.client?.initializeResult?.capabilities;
    const capsJson = caps ? JSON.stringify(caps) : '';
    capsSummary = capsJson
      ? capsJson.length > 120
        ? capsJson.slice(0, 117) + '...'
        : capsJson
      : 'UNAVAILABLE';
  } catch {
    capsSummary = 'UNAVAILABLE';
  }

  const status = a.client?.isRunning() ? 'running' : 'not-started';
  const initializeTs = a.client?.isRunning() ? now : 'NOT INITIALIZED';
  const resolutionMethod = a.ironlsPath
    ? 'PATH or iron.languageServer.path setting'
    : 'NONE';

  const payload = [
    'Iron LSP Diagnose Report',
    '========================',
    `Timestamp:           ${now}`,
    `Extension version:   ${a.extensionVersion}`,
    `Editor:              VSCode ${vscode.version}`,
    `Platform:            ${process.platform}-${process.arch}`,
    '',
    'ironls binary',
    '-------------',
    `Resolved path:       ${a.ironlsPath || 'NOT FOUND'}`,
    `Resolution method:   ${resolutionMethod}`,
    `Version reported:    ${a.ironlsVersion ?? 'UNAVAILABLE'}`,
    `Compatible range:    ${a.compatibleRange || '(none)'}`,
    `SHA-256 (Zed only):  n/a`,
    '',
    'Workspace',
    '---------',
    `Root:                ${folder}`,
    `iron.toml found:     ${ironToml}`,
    `iron.lock found:     ${ironLock}`,
    '',
    'LSP session',
    '-----------',
    `Server capabilities: ${capsSummary}`,
    `Last initialize:     ${initializeTs}`,
    `Status:              ${status}`,
    '',
    'Recent log tail (last 20 lines)',
    '----------------------------------------------------',
    'no log data (VSCode OutputChannel content cannot be read programmatically; open View > Output > "Iron Language Server")',
    '',
  ].join('\n');

  const doc = await vscode.workspace.openTextDocument({
    content: payload,
    language: 'plaintext',
  });
  await vscode.window.showTextDocument(doc, { preview: false });
}

function pathIfExists(p: string): string {
  try {
    return fs.statSync(p).isFile() ? p : 'NO';
  } catch {
    return 'NO';
  }
}
