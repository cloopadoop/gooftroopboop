"""Synthetic ownership checks; never launches or terminates an application."""
import unittest
import os
import subprocess
import sys
from process_ownership import identity, observe, surviving, require_same_identity


def row(pid, created, parent=0, path=r'C:\test\app.exe'):
    return dict(ProcessId=pid, CreationTime=created * 10, ParentProcessId=parent, ExecutablePath=path)


class OwnershipTests(unittest.TestCase):
    def test_valid_descendants(self):
        root, child, grandchild = row(10, 100), row(20, 101, 10), row(30, 102, 20)
        self.assertEqual(surviving([root, child, grandchild], observe([grandchild, child, root], {identity(root)})),
                         [10, 20, 30])

    def test_reused_root_does_not_claim_user_apps(self):
        previous = row(10, 100)
        current = row(10, 200, path=r'C:\other\user.exe')
        user_child = row(20, 201, 10)
        known = observe([current, user_child], {identity(previous)})
        self.assertEqual(surviving([current, user_child], known), [])

    def test_stale_parent_id_does_not_claim_older_child(self):
        root, unrelated = row(10, 200), row(20, 100, 10)
        self.assertEqual(observe([root, unrelated], {identity(root)}), {identity(root)})

    def test_missing_root_cannot_claim_orphan_by_pid(self):
        self.assertEqual(observe([row(20, 101, 10)], {identity(row(10, 100))}), {identity(row(10, 100))})

    def test_previously_observed_orphan_remains_identified(self):
        child = row(20, 101, 10)
        self.assertEqual(surviving([child], {identity(child)}), [20])

    def test_pid_reused_after_observation_is_not_survivor(self):
        self.assertEqual(surviving([row(20, 202, 10)], {identity(row(20, 101, 10))}), [])

    def test_missing_metadata_and_empty_roots(self):
        for process in (row(0, 100), row(10, 0), row(10, 100, path=''), row(10, 100, path='relative.exe')):
            self.assertIsNone(identity(process))
        self.assertEqual(observe([row(20, 101, 10)], set()), set())

    def test_duplicate_pid_fails_closed(self):
        with self.assertRaises(ValueError):
            observe([row(10, 100), row(10, 101)], set())

    def test_unreadable_survivor_is_not_claimed_dead(self):
        with self.assertRaises(RuntimeError):
            surviving([row(10, 100, path='')], {identity(row(10, 100))})

    def test_termination_requires_exact_live_handle_identity(self):
        expected = identity(row(10, 100))
        require_same_identity(expected, row(10, 100))
        for changed in (row(11, 100), row(10, 101), row(10, 100, path=r'C:\other\user.exe'), row(10, 0)):
            with self.assertRaises(RuntimeError):
                require_same_identity(expected, changed)
        with self.assertRaises(RuntimeError):
            require_same_identity(None, row(10, 100))

    @unittest.skipUnless(os.name == 'nt', 'Win32 process-handle control')
    def test_owned_disposable_process_handle(self):
        from native_windows import processes, terminate_verified
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(20)'])
        try:
            matches = [p for p in processes() if p['ProcessId'] == child.pid]
            self.assertEqual(len(matches), 1)
            expected = identity(matches[0])
            self.assertIsNotNone(expected)
            with self.assertRaises(RuntimeError):
                terminate_verified((expected[0], expected[1] + 10000000, expected[2]))
            self.assertIsNone(child.poll())
            terminate_verified(expected)
            self.assertEqual(child.wait(timeout=5), 1)
        finally:
            if child.poll() is None:
                child.terminate()  # Popen retains the exact process handle.
                child.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
