"""Positive and negative controls for the audio comparison gate."""
import unittest
import numpy as np
from scipy import signal
from reference_playback import compare


class ReferenceGateTests(unittest.TestCase):
    def setUp(self):
        rng = np.random.default_rng(31415)
        self.reference = rng.normal(0, 1500, (96000, 2))
        self.filtered = signal.lfilter([0, .25, .5, -.75], [1, -255 / 256], self.reference, axis=0)

    def test_known_filter_and_constant_start_delay(self):
        result = compare(self.reference, np.roll(self.filtered, 3, axis=0))
        self.assertTrue(result["passed"])
        self.assertEqual(result["lag_frames"], 3)

    def test_silence_is_not_a_pass(self):
        with self.assertRaises(ValueError):
            compare(self.reference, np.zeros_like(self.reference))

    def test_missing_channel_is_not_a_pass(self):
        changed = self.filtered.copy()
        changed[:, 1] = 0
        with self.assertRaises(ValueError):
            compare(self.reference, changed)

    def test_wrong_gain_fails_even_with_perfect_correlation(self):
        self.assertFalse(compare(self.reference, self.filtered * .5)["passed"])

    def test_dropped_passage_fails(self):
        changed = self.filtered.copy()
        changed[48000:64000] = 0
        self.assertFalse(compare(self.reference, changed)["passed"])

    def test_wrong_channel_order_fails(self):
        self.assertFalse(compare(self.reference, self.filtered[:, ::-1])["passed"])

    def test_truncated_capture_fails(self):
        with self.assertRaises(ValueError):
            compare(self.reference, self.filtered[:-1])


if __name__ == "__main__":
    unittest.main()
