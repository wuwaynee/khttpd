# SPDX-License-Identifier: MIT
"""Synthetic integrity fixtures; these values are never benchmark evidence."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "bench/diagnose/summarize.py"


def raw(count, rate, p50, errors=0, invalid=0):
    return f"""Running 1s test @ http://127.0.0.1:8081/
  4 threads and 256 connections
    Latency 1.00ms 1.00ms 2.00ms 80.00%
  Latency Distribution
     50% {p50}us
     99% 2.00ms
  {count} requests in 1.00s, 12.00KB read
Requests/sec: {rate:.2f}
Socket errors: connect 0, read {errors}, write 0, timeout 0
KH_VALIDATED checked={count} invalid={invalid}
"""


class DiagnosisEvidenceTests(unittest.TestCase):
    def fixture(self, directory):
        profiles = {name: {"kind": "synthetic"} for name in
                    ("matched_close512", "hardened4")}
        manifest = dict(schema=1, complete=True, source_commit="a" * 40,
            environment={"hz": 100, "uname": "SYNTHETIC TEST FIXTURE"},
            args={"repetitions": 2, "duration": 1}, profiles=profiles, runs=[])
        for rep in range(2):
            for name in profiles:
                rate = 100 if name == "matched_close512" else (200 + 20 * rep)
                row = dict(profile=name, repetition=rep,
                           configuration=profiles[name], complete=True)
                for key, suffix in (("stdout", "wrk"), ("stderr", "stderr"),
                                    ("validation", "validation")):
                    path = directory / f"{rep}-{name}.{suffix}.txt"
                    data = "" if key == "stderr" else raw(rate, rate, 500)
                    path.write_text(data)
                    row[key + "_file"] = path.name
                    row[key + "_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
                for point, ticks in (("before", 10), ("after", 110)):
                    row[point] = dict(proc_stat=f"cpu {ticks} 0 0 100 0 0 0 0\nctxt {ticks}\n",
                                      module={"responses": ticks, "protocol_errors": 0,
                                              "alloc_errors": 0})
                manifest["runs"].append(row)
        self.save(directory, manifest)
        return manifest

    def save(self, directory, manifest):
        (directory / "manifest.json").write_text(json.dumps(manifest))

    def invoke(self, directory):
        return subprocess.run([sys.executable, str(SCRIPT), str(directory)],
                              capture_output=True, text=True)

    def test_recomputes_paired_statistics_from_raw_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory)
            result = self.invoke(directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((directory / "summary.json").read_text())
            self.assertAlmostEqual(summary["paired_comparisons"]["hardened4"]
                                   ["throughput"]["median"], 2.1)
            self.assertEqual(summary["profiles"]["hardened4"]["req_s"]["median"], 210)
            self.assertEqual(summary["profiles"]["hardened4"]["p50_ms"]["median"], .5)

    def test_refuses_tampering_missing_and_duplicate_runs(self):
        for mode in ("tamper", "missing", "duplicate", "incomplete"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                manifest = self.fixture(directory)
                if mode == "tamper":
                    path = directory / manifest["runs"][0]["stdout_file"]
                    path.write_text(path.read_text() + "changed")
                elif mode == "missing":
                    manifest["runs"].pop()
                elif mode == "duplicate":
                    manifest["runs"].append(manifest["runs"][0])
                else:
                    manifest["complete"] = False
                self.save(directory, manifest)
                self.assertNotEqual(self.invoke(directory).returncode, 0)

    def test_socket_errors_remain_visible_and_bad_bodies_fail(self):
        for invalid in (0, 1):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                manifest = self.fixture(directory)
                row = manifest["runs"][0]
                for key in ("stdout", "validation"):
                    path = directory / row[key + "_file"]
                    path.write_text(raw(100, 100, 500, errors=3, invalid=invalid))
                    row[key + "_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
                self.save(directory, manifest)
                result = self.invoke(directory)
                if invalid:
                    self.assertNotEqual(result.returncode, 0)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    summary = json.loads((directory / "summary.json").read_text())
                    self.assertEqual(summary["profiles"]["matched_close512"]
                                     ["socket_error_events"]["maximum"], 3)


if __name__ == "__main__":
    unittest.main()
