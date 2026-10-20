"""Exercise real Result writer/recovery processes in independently owned stores."""

import pathlib
import os
import selectors
import subprocess
import sys
import tempfile
import time
import unittest


HELPER = pathlib.Path(sys.argv.pop(1)).resolve()


class ResultProcessTests(unittest.TestCase):
    def test_live_empty_writer_survives_second_process_and_recovers_after_death(self):
        self.check_writer(0)

    def test_one_committed_bar_recovers_without_future_records(self):
        self.check_writer(1)

    def test_multiple_committed_bars_recover_through_aliased_store(self):
        self.check_writer(2, alias=True)

    def check_writer(self, bars, alias=False):
        with tempfile.TemporaryDirectory(prefix="bte-result-process-") as temporary:
            root = pathlib.Path(temporary)
            child = subprocess.Popen(
                [str(HELPER), "hold", str(root), str(bars)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=False,
            )
            try:
                with selectors.DefaultSelector() as ready:
                    ready.register(child.stdout, selectors.EVENT_READ)
                    deadline = time.monotonic() + 10
                    identity_line = bytearray()
                    while b"\n" not in identity_line:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0 or not ready.select(timeout=remaining):
                            break
                        chunk = os.read(child.stdout.fileno(), 4096)
                        if not chunk:
                            break
                        identity_line.extend(chunk)
                if b"\n" not in identity_line:
                    child.kill()
                    child.wait(timeout=10)
                    self.fail(
                        "writer process did not publish its Result ID: "
                        + child.stderr.read().decode()
                    )
                result_id = identity_line.splitlines()[0].decode().strip()
                self.assertEqual(len(result_id), 32)
                staged = root / "Store" / "Staging" / f"{result_id}.bteresult"
                original = staged.read_bytes()
                inspected_root = root
                if alias:
                    inspected_root = root / "Alias"
                    inspected_root.symlink_to(root, target_is_directory=True)
                inspected = subprocess.run(
                    [str(HELPER), "scan", str(inspected_root), "unused"],
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertEqual(inspected.returncode, 0, inspected.stderr)
                self.assertEqual(inspected.stdout.strip(), "0")
                mutation = subprocess.run(
                    [str(HELPER), "publish", str(inspected_root), result_id],
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertEqual(mutation.returncode, 0, mutation.stderr)
                self.assertEqual(mutation.stdout.strip(), "busy")
                self.assertTrue(staged.exists(), "another process moved a live writer")
                self.assertEqual(staged.read_bytes(), original)
                self.assertEqual(list((root / "Store" / "Quarantine").glob("*.bteresult")), [])
                child.kill()
                child.wait(timeout=10)
                recovered = subprocess.run(
                    [str(HELPER), "recover", str(root), "unused"],
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertEqual(recovered.returncode, 0, recovered.stderr)
                self.assertEqual(recovered.stdout.strip(), f"1 4 {bars}")
                repeated = subprocess.run(
                    [str(HELPER), "scan", str(root), "unused"],
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertEqual(repeated.returncode, 0, repeated.stderr)
                self.assertEqual(repeated.stdout.strip(), "0")
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=10)
                for stream in (child.stdin, child.stdout, child.stderr):
                    stream.close()


if __name__ == "__main__":
    unittest.main()
