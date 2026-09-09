#!/usr/bin/env python3
"""Guard required PR checks against docs-only deadlocks (#116).

Run with: python -m pip install PyYAML==6.0.2
          python tests/ci/test_required_workflows.py

Uses BaseLoader so GitHub's `on` key stays a string (not YAML 1.1 True).
This checks workflow contracts, not a reimplementation of Actions scheduling.
"""

import fnmatch
from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]


def read_workflow(name):
    return yaml.load(
        (ROOT / ".github" / "workflows" / name).read_text(),
        Loader=yaml.BaseLoader,
    )


class RequiredWorkflowTests(unittest.TestCase):
    def test_required_workflows_accept_every_main_pr_change(self):
        for name in ("ci.yml", "release.yml", "build-time.yml"):
            with self.subTest(workflow=name):
                events = read_workflow(name)["on"]
                self.assertIn("pull_request", events)
                trigger = events["pull_request"] or {}
                # No paths, paths-ignore, activity-type filters, or exclusions.
                self.assertLessEqual(set(trigger), {"branches"})
                if "branches" in trigger:
                    self.assertIn("main", trigger["branches"])

    def test_required_ci_jobs_keep_their_names_and_run_for_docs(self):
        jobs = read_workflow("ci.yml")["jobs"]
        self.assertEqual(
            {"ubuntu-latest", "macos-latest"},
            set(jobs["build-and-test"]["strategy"]["matrix"]["os"]),
        )
        for name in ("changes", "build-and-test", "build-and-test-release"):
            with self.subTest(job=name):
                self.assertNotIn("if", jobs[name])
                self.assertNotIn("name", jobs[name])
                self.assertNotIn("continue-on-error", jobs[name])
        for name in ("build-and-test", "build-and-test-release"):
            job = jobs[name]
            self.assertEqual("changes", job["needs"])
            skip_steps = []
            for step in job["steps"]:
                with self.subTest(job=name, step=step.get("name", step.get("uses"))):
                    self.assertNotIn("continue-on-error", step)
                    if step.get("name") == "Skip for docs-only changes":
                        self.assertEqual("needs.changes.outputs.ci != 'true'", step["if"])
                        skip_steps.append(step)
                    else:
                        # Includes dependency installs, checkout, builds and tests.
                        self.assertTrue(step.get("if", "").startswith(
                            "needs.changes.outputs.ci == 'true'"))
            self.assertEqual(1, len(skip_steps))

    def test_path_classification_preserves_docs_and_code_behavior(self):
        steps = read_workflow("ci.yml")["jobs"]["changes"]["steps"]
        filters = next(step["with"]["filters"] for step in steps
                       if step.get("id") == "filter")
        patterns = yaml.load(filters, Loader=yaml.BaseLoader)["ci"]
        # Representative paths use the existing simple globs, not a general
        # picomatch implementation. Changes to those globs need explicit review.
        for path in ("CONTRIBUTING.md", "README.md", "docs/site/index.html"):
            with self.subTest(docs=path):
                self.assertFalse(any(fnmatch.fnmatchcase(path, p) for p in patterns))
        for path in ("src/lir/emit_c.c", "tests/lsp/unit/test_lsp_cancel_request_e2e.c",
                     "tests/ci/test_required_workflows.py", "examples/hello.iron",
                     "CMakeLists.txt", ".github/workflows/ci.yml"):
            with self.subTest(code=path):
                self.assertTrue(any(fnmatch.fnmatchcase(path, p) for p in patterns))

    def test_release_and_build_time_required_matrix_names(self):
        release = read_workflow("release.yml")["jobs"]["build"]
        self.assertNotIn("name", release)
        self.assertNotIn("if", release)
        rows = release["strategy"]["matrix"]["include"]
        self.assertEqual({("ubuntu-24.04", "linux-x86_64"),
                          ("macos-26", "macos-arm64"),
                          ("macos-26-intel", "macos-x86_64")},
                         {(row["os"], row["target"]) for row in rows})
        build_time = read_workflow("build-time.yml")["jobs"]["build-time"]
        self.assertNotIn("name", build_time)
        self.assertNotIn("if", build_time)
        self.assertEqual({"ubuntu-latest", "macos-latest"},
                         set(build_time["strategy"]["matrix"]["os"]))

    def test_contract_tests_run_before_path_filtering(self):
        steps = read_workflow("ci.yml")["jobs"]["changes"]["steps"]
        index = next(i for i, step in enumerate(steps)
                     if "python tests/ci/test_required_workflows.py" in step.get("run", ""))
        self.assertNotIn("if", steps[index])
        self.assertLess(index, next(i for i, step in enumerate(steps)
                                   if step.get("id") == "filter"))


if __name__ == "__main__":
    unittest.main()
