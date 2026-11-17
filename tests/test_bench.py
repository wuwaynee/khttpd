# SPDX-License-Identifier: MIT
"""Synthetic parser fixtures only; these are not benchmark measurements."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from summarize import digest, load_runs, parse_wrk
from run import cpu_set

VALID = """Running 1s test @ http://127.0.0.1:8081/
  1 threads and 2 connections
  Latency Distribution
     50%    500.00us
     99%    2.00ms
  100 requests in 1.00s, 12.00KB read
Requests/sec: 100.00
KH_VALIDATED checked=100 invalid=0
"""


class EvidenceTests(unittest.TestCase):
    def test_units_and_validation(self):
        metrics = parse_wrk(VALID)
        self.assertEqual(metrics["p50_ms"], 0.5)
        self.assertEqual(metrics["p99_ms"], 2)
        self.assertEqual(metrics["req_s"], 100)

    def test_rejects_incomplete_historical_and_erroneous_logs(self):
        bad = [
            VALID.replace("KH_VALIDATED checked=100 invalid=0", ""),
            VALID.replace("checked=100", "checked=99"),
            VALID.replace("invalid=0", "invalid=1"),
            VALID + "Socket errors: connect 0, read 1, write 0, timeout 0\n",
            VALID + "Socket errors: connect 0, read\n",
            VALID + "Non-2xx or 3xx responses: 1\n",
            VALID.replace("2.00ms", "0.1us"),
            VALID.replace("100.00\n", "0.00\n"),
        ]
        for text in bad:
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_wrk(text)

    def make_manifest(self, directory):
        (directory / "raw.txt").write_text(VALID)
        (directory / "err.txt").write_text("")
        return {
            "schema": 1, "complete": True,
            "source": {"clean": True, "commit": "a" * 40, "tree": "b" * 40},
            "args": {"implementation": "userspace", "single": True,
                     "shards": 1, "repetitions": 1},
            "runs": [{
                "configuration": {"implementation": "userspace", "keep_alive": 1,
                                  "shards": 1, "prealloc": 1},
                "repetition": 0, "returncode": 0,
                "stdout": "raw.txt", "stderr": "err.txt",
                "stdout_sha256": digest(directory / "raw.txt"),
                "stderr_sha256": digest(directory / "err.txt"),
            }],
        }

    def test_checksums_completeness_and_missing_cells(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            manifest = self.make_manifest(directory)

            def save():
                (directory / "manifest.json").write_text(json.dumps(manifest))

            save()
            self.assertEqual(len(load_runs(directory)[1]), 1)
            (directory / "raw.txt").write_text(VALID + "tampered")
            with self.assertRaisesRegex(ValueError, "checksum"):
                load_runs(directory)
            (directory / "raw.txt").write_text(VALID)
            manifest["complete"] = False
            save()
            with self.assertRaisesRegex(ValueError, "incomplete"):
                load_runs(directory)
            manifest["complete"] = True
            manifest["args"]["repetitions"] = 2
            save()
            with self.assertRaisesRegex(ValueError, "missing"):
                load_runs(directory)

    def test_cpu_ranges(self):
        self.assertEqual(cpu_set("0-2,4"), {0, 1, 2, 4})
        for text in ("", "2-0", "-1", "1-2-3"):
            with self.assertRaises(ValueError):
                cpu_set(text)


if __name__ == "__main__":
    unittest.main()
