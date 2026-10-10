// A panic on the Iron line under the Visual Studio debugger (#388).
//
// Every Iron panic ends in the C library's abort(). `iron dap` (Linux,
// macOS) selects the Iron frame itself; on Windows the `iron` debug type
// can hand the program to the C/C++ extension's Visual Studio debugger
// (cppvsdbg), which stops in abort() and opens an "abort.cpp not found"
// tab. The panics of a --debug build's checks (assert, an index out of
// bounds, unwrap() of a null Box...) already stop on the Iron line through
// a debug trap compiled into the Iron function; this tracker covers the
// rest (a panic inside the runtime, such as read_file of a missing file,
// and the abort() that follows a trap when the program is continued): on a
// stop whose top frame is in the panic path it focuses the first frame
// whose source is a .iron file, and closes the tab of the missing C source.
//
// Self-contained: it watches only cppvsdbg sessions the `iron` type
// started (their configuration carries IRON_SESSION), whichever way the
// rest of the Windows path goes.

import * as fs from 'node:fs';
import * as vscode from 'vscode';

/** Set on the cppvsdbg configurations the `iron` debug type starts. */
export const IRON_SESSION = 'ironDebug';

/** The functions of the panic path: the C library's abort and what it
 * calls, and the runtime's panic helpers. */
const PANIC_FRAME = /(^|[!\s])(abort|raise|__fastfail|_invoke_watson|_invalid_parameter|iron_panic_\w*|iron_dbg_panic_\w*|iron_oom_abort|iron_abort|Iron_assert|Iron_read_file|iron_debug_panic_stop)\b/;

interface Frame { id: number; name?: string; source?: { path?: string; name?: string } }

function isIron(f: Frame): boolean {
  const p = f.source?.path ?? f.source?.name ?? '';
  return /\.iron$/i.test(p);
}

/** The index of the Iron frame a panic stopped in, or -1 when the top
 * frame is already Iron code or the stop is not in the panic path. */
export function panicFrameIndex(frames: Frame[]): number {
  if (frames.length === 0 || isIron(frames[0])) return -1;
  const i = frames.findIndex(isIron);
  if (i < 0) return -1;
  return frames.slice(0, i).some((f) => PANIC_FRAME.test(f.name ?? '')) ? i : -1;
}

/** Stops that are the program's own doing, not a step or a breakpoint. */
const PANIC_REASONS = new Set(['exception', 'signal', 'unknown', undefined]);

class PanicFocusTracker implements vscode.DebugAdapterTracker {
  constructor(private session: vscode.DebugSession) {}

  onDidSendMessage(m: any): void {
    if (m?.type !== 'event' || m.event !== 'stopped') return;
    const reason = m.body?.reason;
    if (!PANIC_REASONS.has(reason) && reason !== 'breakpoint') return;
    if (reason === 'breakpoint' && (m.body?.hitBreakpointIds ?? []).length > 0) return;
    void this.focusIronFrame(m.body?.threadId).catch(() => undefined);
  }

  private async focusIronFrame(threadId: number | undefined): Promise<void> {
    if (threadId === undefined) return;
    const st = await this.session.customRequest('stackTrace', { threadId, startFrame: 0, levels: 40 });
    const frames: Frame[] = st?.stackFrames ?? [];
    const i = panicFrameIndex(frames);
    if (i < 0) return;
    const target = frames[i];
    const stale = frames.slice(0, i).map((f) => f.source?.path).filter((p): p is string => !!p);
    // VS Code focuses the top frame once it has the stack; then walk down
    // the call stack to the Iron frame (there is no API to focus a frame).
    for (let tries = 0; tries < 50; tries++) {
      const active = vscode.debug.activeStackItem;
      if (active instanceof vscode.DebugStackFrame && active.session.id === this.session.id &&
          active.threadId === threadId) {
        if (active.frameId === target.id) break;
        await vscode.commands.executeCommand('workbench.action.debug.callStackDown');
      }
      await new Promise((r) => setTimeout(r, 100));
    }
    await closeMissingSources(stale);
  }
}

/** Close the editors VS Code opened for the C sources of the panic path
 * (abort.cpp and the like), which are not on this machine. */
async function closeMissingSources(paths: string[]): Promise<void> {
  const names = new Set(paths.map((p) => p.replace(/\\/g, '/').split('/').pop()!.toLowerCase()));
  const tabs: vscode.Tab[] = [];
  for (const group of vscode.window.tabGroups.all) {
    for (const tab of group.tabs) {
      const input = tab.input;
      if (!(input instanceof vscode.TabInputText)) continue;
      const base = input.uri.path.split('/').pop()?.toLowerCase() ?? '';
      if (!names.has(base)) continue;
      if (input.uri.scheme === 'debug' || (input.uri.scheme === 'file' && !fs.existsSync(input.uri.fsPath))) {
        tabs.push(tab);
      }
    }
  }
  if (tabs.length > 0) await vscode.window.tabGroups.close(tabs, true);
}

export function registerPanicFocus(context: vscode.ExtensionContext): void {
  context.subscriptions.push(vscode.debug.registerDebugAdapterTrackerFactory('cppvsdbg', {
    createDebugAdapterTracker: (session) =>
      session.configuration?.[IRON_SESSION] ? new PanicFocusTracker(session) : undefined,
  }));
}
