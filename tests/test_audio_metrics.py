"""Fail-closed controls for the program-change audio regression metric."""
import io
import math
import struct
import unittest
import wave

from regression import read_wav_mono_from_bytes, spectral_distance_db


def tone(frames=2048, frequency=440, amplitude=12000, right_gain=1, rate=32000):
    data = io.BytesIO()
    with wave.open(data, "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        pcm = [int(amplitude * math.sin(2 * math.pi * frequency * i / 32000)) for i in range(frames)]
        wav.writeframes(b"".join(struct.pack("<hh", value, int(value * right_gain)) for value in pcm))
    return data.getvalue()


class AudioMetricTests(unittest.TestCase):
    def test_identical_signal_passes_and_octave_change_fails(self):
        original = tone()
        self.assertLess(spectral_distance_db(original, original), -100)
        self.assertGreater(spectral_distance_db(original, tone(frequency=880)), -15)

    def test_missing_signal_and_shortened_output_fail(self):
        original = tone()
        for other in (tone(amplitude=0), tone(frames=1024), tone(frames=0)):
            with self.subTest(length=len(other)):
                self.assertEqual(spectral_distance_db(original, other), math.inf)
        self.assertEqual(spectral_distance_db(tone(amplitude=0), tone(amplitude=0)), math.inf)

    def test_exactly_one_analysis_window_is_checked(self):
        original = tone(frames=1024)
        self.assertLess(spectral_distance_db(original, original), -100)
        self.assertGreater(spectral_distance_db(original, tone(frames=1024, frequency=880)), -15)

    def test_right_channel_loss_and_sample_rate_change_fail(self):
        original = tone()
        self.assertEqual(spectral_distance_db(original, tone(right_gain=0)), math.inf)
        self.assertEqual(spectral_distance_db(original, tone(rate=44100)), math.inf)

    def test_wav_headers_and_truncation_are_validated(self):
        with self.assertRaises((wave.Error, EOFError, ValueError)):
            read_wav_mono_from_bytes(b"not a WAV")
        with self.assertRaises(ValueError):
            read_wav_mono_from_bytes(tone()[:-4])


if __name__ == "__main__":
    unittest.main()
