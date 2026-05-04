// Phase 6 Plan 06-03 Task 2 (EXT-04, UI-SPEC S5): structured JSON logging
// helper. Each line is a single-object JSON record matching the UI-SPEC S5
// shape { ts, lvl, src, evt, ...extra }. The source tag `vscode-ext`
// distinguishes editor-side events from ironls server events (which use
// src: `ironls`).

import type { OutputChannel } from 'vscode';

export type LogLevel = 'error' | 'warn' | 'info' | 'debug';

// S5 event vocabulary. Kept as a const tuple so tests + callers can
// enumerate valid events.
//
// Phase 7 Plan 07-07 (HARD-22, D-10): `ironls.version_mismatch` is
// emitted from extension.ts when the detected ironls version falls
// outside `ironLspCompatibleIronlsRange` or cannot be probed. It is
// the structured counterpart to the UI-SPEC S9 red toast shown on
// hard-refuse activation.
export const EVENTS = [
  'ext.activate',
  'ext.deactivate',
  'ironls.discovered',
  'ironls.not_found',
  'ironls.version_check',
  'ironls.version_mismatch',
  'ironls.spawn.ok',
  'ironls.spawn.failed',
  'lsp.initialize.start',
  'lsp.initialize.ok',
  'lsp.initialize.failed',
  'lsp.shutdown',
] as const;

export type EventName = (typeof EVENTS)[number];

/**
 * Append a single UI-SPEC S5 log line to the given OutputChannel.
 *
 * The shape is exactly:
 *   {"ts":"ISO","lvl":"info","src":"vscode-ext","evt":"<event>", ...extra}
 */
export function logEvent(
  channel: OutputChannel,
  lvl: LogLevel,
  evt: EventName,
  extra: Record<string, unknown> = {}
): void {
  const record = {
    ts: new Date().toISOString(),
    lvl,
    src: 'vscode-ext',
    evt,
    ...extra,
  };
  try {
    channel.appendLine(JSON.stringify(record));
  } catch {
    // OutputChannel can be disposed between activate and deactivate; fall
    // back to console so we never crash the extension host on a log call.
    // eslint-disable-next-line no-console
    console.error('[iron-lsp] logEvent after dispose', record);
  }
}

export interface Logger {
  error(evt: EventName, extra?: Record<string, unknown>): void;
  warn(evt: EventName, extra?: Record<string, unknown>): void;
  info(evt: EventName, extra?: Record<string, unknown>): void;
  debug(evt: EventName, extra?: Record<string, unknown>): void;
}

export function createLogger(channel: OutputChannel): Logger {
  return {
    error: (evt, extra) => logEvent(channel, 'error', evt, extra),
    warn: (evt, extra) => logEvent(channel, 'warn', evt, extra),
    info: (evt, extra) => logEvent(channel, 'info', evt, extra),
    debug: (evt, extra) => logEvent(channel, 'debug', evt, extra),
  };
}
