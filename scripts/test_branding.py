#!/usr/bin/env python3
"""Regression tests for branding checks; run with python3 scripts/test_branding.py."""

from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

from check_branding import EXAMPLES, run_examples, validate_page, validate_repository


class BrandingTests(unittest.TestCase):
    def test_highlighting_and_entities_preserve_source(self):
        self.assertEqual(validate_page(
            '<pre data-example="example"><span>val</span> x = &quot;a &amp; b&quot;</pre>',
            {"example"}, {"example": 'val x = "a & b"\n'}), [])

    def test_changed_example_is_rejected(self):
        self.assertTrue(validate_page('<pre data-example="example">bad</pre>',
                                      {"example"}, {"example": "good"}))

    def test_missing_example_is_rejected(self):
        self.assertTrue(validate_page("<p>No code</p>", {"example"}, {"example": "code"}))

    def test_unknown_example_is_rejected(self):
        self.assertTrue(validate_page('<pre data-example="typo">code</pre>', set(), {}))

    def test_duplicate_example_is_rejected(self):
        self.assertTrue(validate_page('<pre data-example="a">x</pre>' * 2, {"a"}, {"a": "x"}))

    def test_old_headline_and_metadata_are_rejected(self):
        for content in ('<h1>Forged <span>for games</span></h1>',
                        '<meta name="description" content="A language for game development">'):
            with self.subTest(content=content):
                self.assertTrue(validate_page(content, set(), {}))

    def test_games_as_a_use_case_is_allowed(self):
        self.assertEqual(validate_page('<p>Tools, services, simulations, and games.</p>', set(), {}), [])

    def test_current_repository(self):
        root = Path(__file__).resolve().parent.parent
        self.assertEqual(validate_repository(root, root / "docs/site"), [])

    def test_missing_repository_reports_errors(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temporary:
            self.assertTrue(validate_repository(Path(temporary), Path(temporary)))

    @patch("check_branding.subprocess.run")
    def test_execution_checks_exact_output_and_exit(self, run):
        for code, output in ((1, ""), (0, "wrong\n")):
            run.return_value = subprocess.CompletedProcess([], code, output, "diagnostic")
            self.assertEqual(len(run_examples(Path("/repo"), Path("/iron"))), len(EXAMPLES))

    @patch("check_branding.subprocess.run")
    def test_execution_success(self, run):
        run.side_effect = [subprocess.CompletedProcess([], 0, output, "") for output in EXAMPLES.values()]
        self.assertEqual(run_examples(Path("/repo"), Path("/iron")), [])
        self.assertTrue(all(call.kwargs["timeout"] == 90 for call in run.call_args_list))

    @patch("check_branding.subprocess.run", side_effect=subprocess.TimeoutExpired("iron", 90))
    def test_execution_timeout_is_reported(self, run):
        self.assertEqual(len(run_examples(Path("/repo"), Path("/iron"))), len(EXAMPLES))


if __name__ == "__main__":
    unittest.main()
