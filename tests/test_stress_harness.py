"""Stress prerequisites fail before a misleading campaign can start."""
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from stress import check_model_startup


class StressHarnessTests(unittest.TestCase):
    def test_missing_runtime_is_actionable(self):
        result = SimpleNamespace(returncode=0xC0000135, stderr=b"")
        with patch("stress.subprocess.run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "C0000135"):
                check_model_startup(Path("model.exe"))

    def test_expected_usage_exit_proves_binary_started(self):
        result = SimpleNamespace(returncode=2, stderr=b"Usage: gtb-model-test <file>")
        with patch("stress.subprocess.run", return_value=result):
            check_model_startup(Path("model.exe"))


if __name__ == "__main__":
    unittest.main()
