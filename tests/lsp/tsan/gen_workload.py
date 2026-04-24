#!/usr/bin/env python3
"""Phase 7 Plan 07-03 Task 01 (HARD-17, D-05) -- TSAN workload generator.

Emits exactly 300 newline-delimited JSON events to stdout. Deterministic:
seed is fixed so regenerating produces byte-identical output, which is
verified in CI via `wc -l` + the committed file (workload.jsonl).

Event distribution targets the five highest-risk threading surfaces from
Phase 2-5 (per 07-03-PLAN.md <action>):

  Events   1- 60  didChange bursts on doc_a.iron
                  -> writer queue + coalescing mailbox stress
  Events  61-120  interleaved hover+completion on doc_a with didChange
                  bursts on doc_b.iron
                  -> cancel registry + per-request arena stress
  Events 121-180  workspace/symbol queries concurrent with
                  didChangeWatchedFiles
                  -> workspace index lock stress (NAV-01)
  Events 181-240  textDocument/definition cross-refs while doc_b edited
                  -> facade concurrent analyze stress
  Events 241-300  rapid-fire $/cancelRequest mid-compile
                  -> cancel poll boundary stress (Phase 3 D-16)

Every event object has:
    {"id": <int|None>, "method": <str>, "params": <obj>}

The driver (tests/lsp/tsan/driver.py) consumes this file, applies
LSP framing, and ships events in parallel over two workers.

Usage:
    python3 tests/lsp/tsan/gen_workload.py > tests/lsp/tsan/workload.jsonl
    wc -l tests/lsp/tsan/workload.jsonl  # must equal 300
"""
from __future__ import annotations

import json
import random
import sys


SEED = 20260423  # Phase 7 Plan 07-03 land date; fixed so regen is stable.

DOC_A_URI = "file:///fixtures/doc_a.iron"
DOC_B_URI = "file:///fixtures/doc_b.iron"

TOTAL_EVENTS = 300
BURST_A_END = 60     # events 1..60
BURST_INTERLEAVE_END = 120
WORKSPACE_END = 180
DEFINITION_END = 240
CANCEL_END = 300


def make_event(method: str, params: dict, *, request_id=None) -> dict:
    ev = {"method": method, "params": params}
    if request_id is not None:
        ev["id"] = request_id
    return ev


def gen() -> list:
    rng = random.Random(SEED)
    events: list = []
    version = 1
    next_id = 1000

    # Segment 1 (1..60): didChange bursts on doc_a.iron
    for _ in range(BURST_A_END):
        version += 1
        params = {
            "textDocument": {"uri": DOC_A_URI, "version": version},
            "contentChanges": [
                {"text": f"// burst change #{version}\n"
                         f"val counter: Int = {rng.randint(1, 1000)}\n"}
            ],
        }
        events.append(make_event("textDocument/didChange", params))

    # Segment 2 (61..120): interleaved hover + completion on doc_a
    # with didChange bursts on doc_b.iron. Stresses cancel registry +
    # per-request arena (each hover/completion gets its own arena).
    for i in range(BURST_INTERLEAVE_END - BURST_A_END):
        if i % 3 == 0:
            version += 1
            params = {
                "textDocument": {"uri": DOC_B_URI, "version": version},
                "contentChanges": [
                    {"text": f"// interleave change #{version}\n"
                             f"val flag_{i}: Int = {rng.randint(1, 999)}\n"}
                ],
            }
            events.append(make_event("textDocument/didChange", params))
        elif i % 3 == 1:
            next_id += 1
            params = {
                "textDocument": {"uri": DOC_A_URI},
                "position": {"line": rng.randint(10, 30),
                             "character": rng.randint(2, 20)},
            }
            events.append(make_event("textDocument/hover", params,
                                     request_id=next_id))
        else:
            next_id += 1
            params = {
                "textDocument": {"uri": DOC_A_URI},
                "position": {"line": rng.randint(10, 30),
                             "character": rng.randint(2, 20)},
                "context": {"triggerKind": 1},
            }
            events.append(make_event("textDocument/completion", params,
                                     request_id=next_id))

    # Segment 3 (121..180): workspace/symbol concurrent with
    # didChangeWatchedFiles. This is where the workspace index lock
    # (src/lsp/store/workspace_index.c, NAV-01) gets contested.
    for i in range(WORKSPACE_END - BURST_INTERLEAVE_END):
        if i % 2 == 0:
            next_id += 1
            query = rng.choice(["DOC_", "helper_", "chain_", "compute_",
                                "label_", "doc_"])
            params = {"query": query}
            events.append(make_event("workspace/symbol", params,
                                     request_id=next_id))
        else:
            # didChangeWatchedFiles is a notification. Type 2 == Change.
            which = rng.choice([DOC_A_URI, DOC_B_URI])
            params = {"changes": [{"uri": which, "type": 2}]}
            events.append(make_event("workspace/didChangeWatchedFiles",
                                     params))

    # Segment 4 (181..240): textDocument/definition on doc_a->doc_b
    # cross-refs while doc_b is being edited. Stresses the facade
    # concurrent analyze path (src/lsp/facade/).
    for i in range(DEFINITION_END - WORKSPACE_END):
        if i % 4 == 3:
            version += 1
            params = {
                "textDocument": {"uri": DOC_B_URI, "version": version},
                "contentChanges": [
                    {"text": f"// xref edit #{version}\n"
                             f"val xref_tag_{i}: Int = {rng.randint(1, 500)}\n"}
                ],
            }
            events.append(make_event("textDocument/didChange", params))
        else:
            next_id += 1
            # Positions chosen to land on `doc_b.DOC_B_ID` / `doc_b.helper_b`
            # references in doc_a.iron (lines ~18-24 from the fixture).
            params = {
                "textDocument": {"uri": DOC_A_URI},
                "position": {"line": rng.randint(17, 30),
                             "character": rng.randint(10, 30)},
            }
            events.append(make_event("textDocument/definition", params,
                                     request_id=next_id))

    # Segment 5 (241..300): rapid-fire $/cancelRequest mid-compile.
    # Each cancel targets a recent request id. Stresses the Phase 3
    # D-16 cancel poll boundary (src/lsp/server/cancel.c): compile
    # workers must observe the cancel flag without tearing.
    cancel_window_start = next_id - 50  # last ~50 requests
    for i in range(CANCEL_END - DEFINITION_END):
        if i % 5 == 0:
            # Mix in a new request that the next cancel can chase.
            next_id += 1
            params = {
                "textDocument": {"uri": DOC_A_URI},
                "position": {"line": rng.randint(10, 30),
                             "character": rng.randint(2, 20)},
            }
            events.append(make_event("textDocument/hover", params,
                                     request_id=next_id))
        else:
            target = rng.randint(cancel_window_start, max(cancel_window_start,
                                                           next_id))
            params = {"id": target}
            events.append(make_event("$/cancelRequest", params))

    assert len(events) == TOTAL_EVENTS, \
        f"workload generator produced {len(events)} events, expected {TOTAL_EVENTS}"
    return events


def main() -> int:
    events = gen()
    out = sys.stdout
    for ev in events:
        out.write(json.dumps(ev, sort_keys=True))
        out.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
