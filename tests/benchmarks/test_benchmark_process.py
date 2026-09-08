#!/usr/bin/env python3
"""Regression tests for the process boundary used by every benchmark sample."""

import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from benchmark_process import rss_kib, run

RUNNER = Path(__file__).with_name("benchmark_process.py")
HAS_RSS = sys.platform == "darwin" or os.access("/usr/bin/time", os.X_OK)


class BenchmarkProcessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="iron_bench_test_")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "output with spaces"
        self.memory = self.root / "memory"

    def command(self, program, memory=False, seconds="2"):
        args = [sys.executable, str(RUNNER), "--timeout", seconds,
                "--output", str(self.output)]
        if memory:
            args += ["--memory", str(self.memory)]
        return args + ["--", sys.executable, "-c", program]

    def execute(self, program, **kwargs):
        return subprocess.run(self.command(program, **kwargs),
                              capture_output=True, timeout=8)

    def test_success_stdout_stderr_and_memory(self):
        result = self.execute("import sys; print('stdout'); print('stderr', file=sys.stderr)",
                              memory=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(self.output.read_text().splitlines()), {"stdout", "stderr"})
        if HAS_RSS:
            self.assertGreater(int(self.memory.read_text()), 0)
        else:
            self.assertEqual(int(self.memory.read_text()), 0)

    def test_exit_status_preserved_with_and_without_memory(self):
        for memory in (False, True):
            with self.subTest(memory=memory):
                self.assertEqual(self.execute("raise SystemExit(37)", memory=memory).returncode, 37)

    def test_timeout_with_and_without_memory(self):
        for memory in (False, True):
            with self.subTest(memory=memory):
                start = time.monotonic()
                result = self.execute("import time; print('started', flush=True); time.sleep(60)",
                                      seconds="0.2", memory=memory)
                self.assertEqual(result.returncode, 124, result.stderr)
                self.assertLess(time.monotonic() - start, 4)
                self.assertIn("started", self.output.read_text())
                if memory:
                    # A killed GNU time cannot finish its RSS report.
                    self.assertGreaterEqual(int(self.memory.read_text()), 0)

    def test_signalled_child(self):
        for memory in (False, True):
            with self.subTest(memory=memory):
                self.assertEqual(self.execute("import os, signal; os.kill(os.getpid(), signal.SIGTERM)",
                                              memory=memory).returncode,
                                 128 + signal.SIGTERM)

    def test_missing_executable(self):
        args = self.command("", memory=True)
        args[args.index("--") + 1:] = [str(self.root / "nonexistent")]
        result = subprocess.run(args, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 127)
        self.assertIn("nonexistent", self.output.read_text())
        self.assertTrue(self.memory.exists())

    def test_invalid_timeouts(self):
        for seconds in ("0", "-1", "nan", "inf"):
            with self.subTest(seconds=seconds):
                self.assertEqual(self.execute("print('must not run')", seconds=seconds).returncode, 2)
                self.assertFalse(self.output.exists())

    def test_rss_units(self):
        self.assertEqual(rss_kib(8192, "darwin"), 8)
        self.assertEqual(rss_kib(8192, "linux"), 8192)

    @unittest.skipUnless(HAS_RSS, "GNU time is required for Linux RSS measurement")
    def test_memory_measures_each_child_independently(self):
        self.assertEqual(self.execute("data = bytearray(64 * 1024 * 1024)", memory=True).returncode, 0)
        large = int(self.memory.read_text())
        self.assertEqual(self.execute("pass", memory=True).returncode, 0)
        small = int(self.memory.read_text())
        self.assertGreater(large, small + 32 * 1024)

    @unittest.skipUnless(HAS_RSS, "GNU time is required for Linux RSS measurement")
    def test_memory_excludes_launcher_high_water_mark(self):
        script = (f"import sys; sys.path.insert(0, {str(RUNNER.parent)!r}); "
                  "from benchmark_process import run; "
                  "data = bytearray(64 * 1024 * 1024); "
                  f"sys.exit(run(2, {str(self.output)!r}, ['/bin/sleep', '0.01'], {str(self.memory)!r}))")
        result = subprocess.run([sys.executable, "-c", script], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        peak = int(self.memory.read_text())
        self.assertGreater(peak, 0)
        self.assertLess(peak, 32 * 1024, "benchmark RSS must not include its Python launcher")

    def test_unavailable_linux_sampler_preserves_execution(self):
        with patch("benchmark_process.sys.platform", "linux"), \
                patch("benchmark_process.os.path.isfile", return_value=False):
            rc = run(2, self.output, [sys.executable, "-c", "print('still runs')"], self.memory)
        self.assertEqual(rc, 0)
        self.assertEqual(self.output.read_text(), "still runs\n")
        self.assertEqual(int(self.memory.read_text()), 0)

    def assert_stopped(self, pid):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            result = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)],
                                    capture_output=True, text=True)
            state = result.stdout.strip()
            # A killed grandchild may briefly be a zombie awaiting init.
            # An empty/erroring ps result alone is not proof of cleanup.
            if result.returncode == 0 and state.startswith("Z"):
                return
            time.sleep(0.02)
        self.fail(f"descendant {pid} is still running")

    def test_descendants_cleaned_on_timeout_and_normal_exit(self):
        for suffix in ("import time; time.sleep(60)", "pass"):
            with self.subTest(suffix=suffix):
                program = ("import subprocess, sys; "
                           "p = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']); "
                           "print(p.pid, flush=True); " + suffix)
                result = self.execute(program, seconds="0.3", memory=True)
                self.assertEqual(result.returncode, 124 if suffix != "pass" else 0)
                self.assert_stopped(int(self.output.read_text()))

    def test_cancellation_cleans_process_group(self):
        program = ("import subprocess, sys, time; "
                   "p = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']); "
                   "print(p.pid, flush=True); time.sleep(60)")
        runner = subprocess.Popen(self.command(program, memory=True, seconds="60"))
        try:
            deadline = time.monotonic() + 4
            while not self.output.exists() or not self.output.read_text().strip():
                self.assertLess(time.monotonic(), deadline, "child did not start")
                time.sleep(0.01)
            child_pid = int(self.output.read_text())
            runner.send_signal(signal.SIGTERM)
            self.assertEqual(runner.wait(timeout=4), 128 + signal.SIGTERM)
            self.assert_stopped(child_pid)
            self.assertTrue(self.memory.exists())
        finally:
            if runner.poll() is None:
                runner.kill()
                runner.wait()

    def test_cancellation_during_launch_reaps_child(self):
        real_popen = subprocess.Popen
        children = []

        def launch_then_cancel(*args, **kwargs):
            child = real_popen(*args, **kwargs)
            children.append(child)
            os.kill(os.getpid(), signal.SIGTERM)
            return child

        try:
            with patch("benchmark_process.subprocess.Popen", side_effect=launch_then_cancel):
                rc = run(2, self.output, [sys.executable, "-c", "import time; time.sleep(60)"])
            self.assertEqual(rc, 128 + signal.SIGTERM)
            self.assertEqual(len(children), 1)
            self.assertIsNotNone(children[0].returncode, "launcher must reap the child")
            self.assert_stopped(children[0].pid)
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                    child.wait()


if __name__ == "__main__":
    unittest.main()
