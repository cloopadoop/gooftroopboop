"""Independent editor instances must never share staging files or song bytes."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path(os.environ.get("GTB_TEST_ENGINE", ROOT / "build/win-x64/bin/gtb-engine.exe"))
BANK = Path(os.environ.get("GTB_SOUNDBANK", ROOT.parents[1] / "Music Sources/SPC/08 Hamlet.spc"))


class InstanceIsolationTests(unittest.TestCase):
    def setUp(self):
        if not ENGINE.is_file() or not BANK.is_file():
            self.fail("Instance isolation requires the built engine and local soundbank")
        self.temp = tempfile.TemporaryDirectory(prefix="gtb-instance-tests-")
        self.addCleanup(self.temp.cleanup)
        self.env = dict(os.environ, TEMP=self.temp.name, TMP=self.temp.name, GTB_SOUNDBANK=str(BANK))

    def test_live_instances_own_distinct_backing_files(self):
        # communicate returns only after EOF, so use one worker per live process
        # and observe staging while a second open remains alive. A ready marker
        # is its complete JSON response, not a timing-based sleep.
        processes = []
        try:
            for _ in range(2):
                process = subprocess.Popen([str(ENGINE)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                           stderr=subprocess.DEVNULL, text=True, cwd=ROOT, env=self.env)
                processes.append(process)
                process.stdin.write('{"cmd":"new"}\n')
                process.stdin.flush()
                with ThreadPoolExecutor(max_workers=1) as pool:
                    future = pool.submit(process.stdout.readline)
                    try:
                        response = future.result(timeout=20)
                    except TimeoutError:
                        process.kill()
                        raise AssertionError("Engine did not acknowledge new song") from None
                self.assertTrue(json.loads(response).get("ok"), "new song failed")
            files = list(Path(self.temp.name).glob("*.aram"))
            self.assertEqual(len(files), 2, "live engines must own two distinct backing files")
            self.assertTrue(all(path.stat().st_size == 65536 for path in files))
            processes[0].communicate(timeout=20)
            remaining = list(Path(self.temp.name).glob("*.aram"))
            self.assertEqual(len(remaining), 1, "closing one engine must preserve the other's file")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=10)

    def test_parallel_new_edit_session_roundtrips_remain_independent(self):
        def exercise(index):
            pitch = 55 + index
            requests = []
            for cycle in range(12):
                session = str(Path(self.temp.name) / f"song-{index}-{cycle}.gtb")
                requests.extend([
                    {"cmd": "new"},
                    {"cmd": "insertNote", "track": 0, "tick": 0, "pitch": pitch, "len": 24},
                    {"cmd": "saveSession", "path": session},
                    {"cmd": "openSession", "path": session},
                ])
            process = subprocess.run([str(ENGINE)], input="".join(json.dumps(r) + "\n" for r in requests),
                                     capture_output=True, text=True, timeout=120, cwd=ROOT, env=self.env)
            self.assertEqual(process.returncode, 0, "engine process failed")
            replies = [json.loads(line) for line in process.stdout.splitlines()]
            self.assertEqual(len(replies), len(requests))
            self.assertTrue(all(reply.get("ok") for reply in replies), "parallel operation failed")
            for reply in replies[3::4]:
                notes = [n for t in reply["state"]["tracks"] for n in t["notes"] if not n["rest"]]
                self.assertEqual([(n["pitch"], n["len"]) for n in notes], [(pitch, 24)])
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(exercise, range(4)))
        self.assertEqual(list(Path(self.temp.name).glob("*.aram")), [], "normal exit must remove backing files")


if __name__ == "__main__":
    unittest.main()
