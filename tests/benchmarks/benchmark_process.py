#!/usr/bin/env python3
"""Run one benchmark in a bounded process group, optionally recording peak RSS.

No /usr/bin/time feature probe: macOS uses child resource accounting without
sysctls. Linux uses GNU time to avoid counting the Python launcher's inherited
RSS high-water mark as part of a tiny benchmark's memory usage.
Benchmark timings come from the program output, not this launcher's wall time.
"""

import argparse
import math
import os
import resource
import signal
import subprocess
import sys
import tempfile
from pathlib import Path


class Interrupted(Exception):
    def __init__(self, signum):
        self.signum = signum


def rss_kib(peak, platform):
    return int(peak / 1024) if platform == "darwin" else int(peak)


def run(seconds, output, command, memory=None):
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("timeout must be finite and positive")
    proc = None
    cancellation = None
    previous = {}
    measurement = None
    rss_file = None
    actual_command = command
    if (memory is not None and sys.platform.startswith("linux")
            and os.path.isfile("/usr/bin/time") and os.access("/usr/bin/time", os.X_OK)):
        measurement = tempfile.TemporaryDirectory(prefix="iron_bench_rss_")
        rss_file = Path(measurement.name) / "rss"
        actual_command = ["/usr/bin/time", "-q", "-f", "%M", "-o", str(rss_file), "--"] + command

    def interrupted(signum, _frame):
        nonlocal cancellation
        cancellation = signum
        # Popen must finish assigning the child's PID before we unwind;
        # otherwise a cancellation during launch could orphan that child.
        if proc is not None:
            raise Interrupted(signum)

    try:
        for sig in (signal.SIGTERM, signal.SIGINT):
            previous[sig] = signal.signal(sig, interrupted)
        with open(output, "wb") as stream:
            if cancellation is not None:
                raise Interrupted(cancellation)
            try:
                proc = subprocess.Popen(actual_command, stdout=stream,
                                        stderr=subprocess.STDOUT,
                                        start_new_session=True)
            except OSError as error:
                stream.write((str(error) + "\n").encode())
                return 127
            if cancellation is not None:
                raise Interrupted(cancellation)
            try:
                rc = proc.wait(timeout=seconds)
                return rc if rc >= 0 else 128 - rc
            except subprocess.TimeoutExpired:
                return 124
    except Interrupted as error:
        return 128 + error.signum
    finally:
        # Clean up descendants even if their leader exited successfully.
        # Ignore further cancellation until the leader is reaped.
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        if proc is not None:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        try:
            if memory is not None:
                if sys.platform.startswith("linux"):
                    # A timed-out/killed sampler may not have written stats.
                    # Missing GNU time likewise means unavailable, not the
                    # misleading inherited Python RSS. Record 0 in both cases.
                    try:
                        peak = int(rss_file.read_text()) if rss_file else 0
                    except (OSError, ValueError):
                        peak = 0
                else:
                    peak = rss_kib(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
                                   sys.platform)
                Path(memory).write_text(str(peak) + "\n")
        finally:
            if measurement is not None:
                measurement.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", required=True, type=float)
    parser.add_argument("--output", required=True)
    parser.add_argument("--memory")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("a command is required")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("timeout must be finite and positive")
    return run(args.timeout, args.output, command, args.memory)


if __name__ == "__main__":
    sys.exit(main())
