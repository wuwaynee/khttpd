#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Parse captured wrk evidence strictly. Never infer missing measurements."""
import argparse
import csv
import hashlib
import json
import itertools
import math
from pathlib import Path
import re
import statistics
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def parse_wrk(text):
    def match(pattern):
        result = re.search(pattern, text, re.MULTILINE)
        if result is None:
            raise ValueError("missing wrk field: " + pattern)
        return result

    count = int(match(r"^\s*(\d+) requests in [0-9.]+s,").group(1))
    rate = float(match(r"^Requests/sec:\s*([0-9.]+)\s*$").group(1))
    units = {"us": 0.001, "ms": 1.0, "s": 1000.0, "m": 60000.0}

    def latency(percent):
        m = match(r"^\s*" + str(percent) + r"%\s+([0-9.]+)(us|ms|s|m)\s*$")
        return float(m.group(1)) * units[m.group(2)]

    checked, invalid = map(int, match(
        r"^KH_VALIDATED checked=(\d+) invalid=(\d+)\s*$").groups())
    socket_pattern = r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)"
    if "Socket errors:" in text and not re.search(socket_pattern, text):
        raise ValueError("malformed socket error summary")
    for m in re.finditer(socket_pattern, text):
        if any(map(int, m.groups())):
            raise ValueError("wrk socket errors")
    m = re.search(r"Non-2xx or 3xx responses:\s*(\d+)", text)
    if m and int(m.group(1)):
        raise ValueError("non-success responses")
    p50, p99 = latency(50), latency(99)
    if invalid or checked != count or count <= 0:
        raise ValueError("response validation failed or count mismatch")
    if not all(math.isfinite(x) and x >= 0 for x in (rate, p50, p99)) or rate <= 0:
        raise ValueError("invalid metric")
    if p99 < p50:
        raise ValueError("inconsistent percentiles")
    return {"requests": count, "req_s": rate, "p50_ms": p50, "p99_ms": p99}


def load_runs(directory):
    directory = Path(directory).resolve()
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest.get("schema") != 1 or not manifest.get("complete"):
        raise ValueError("incomplete or unsupported run manifest")
    if not manifest.get("source", {}).get("clean"):
        raise ValueError("source was not recorded as clean")
    source = manifest["source"]
    if any(not re.fullmatch(r"[0-9a-f]{40}", source.get(k, ""))
           for k in ("commit", "tree")):
        raise ValueError("missing source identifiers")
    args = manifest["args"]
    impls = ["userspace", "kernel"] if args["implementation"] == "both" else [
        args["implementation"]]
    cells = [(1, 1, 1)] if args["single"] else list(itertools.product(
        (0, 1), (1, args["shards"]) if args["shards"] != 1 else (1,), (0, 1)))
    expected = {(impl, keep, shard, pre, rep) for impl in impls
                for keep, shard, pre in cells for rep in range(args["repetitions"])}
    seen = set()
    records = []
    for record in manifest["runs"]:
        for field in ("stdout", "stderr"):
            path = (directory / record[field]).resolve()
            if path.parent != directory or digest(path) != record[field + "_sha256"]:
                raise ValueError("artifact path/checksum mismatch")
        if record["returncode"] != 0:
            raise ValueError("load generator failed")
        metrics = parse_wrk((directory / record["stdout"]).read_text())
        config = record["configuration"]
        key = tuple(config[k] for k in ("implementation", "keep_alive", "shards", "prealloc"))
        key += (record["repetition"],)
        if key in seen:
            raise ValueError("duplicate run")
        seen.add(key)
        records.append({**config, "repetition": record["repetition"], **metrics})
    if seen != expected:
        raise ValueError("missing or unexpected experiment cells")
    if not records:
        raise ValueError("no measurements")
    return manifest, records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--publish", action="store_true",
                        help="require at least five independent runs per cell")
    args = parser.parse_args()
    manifest, records = load_runs(args.directory)
    groups = {}
    for r in records:
        key = tuple(r[k] for k in ("implementation", "keep_alive", "shards", "prealloc"))
        groups.setdefault(key, []).append(r)
    if args.publish and (manifest["args"]["repetitions"] < 5 or any(
        len({r["repetition"] for r in rows}) < 5 for rows in groups.values()
    )):
        raise ValueError("publication requires >=5 repetitions per cell")
    writer = csv.writer(sys.stdout)
    writer.writerow(["implementation", "keep_alive", "shards", "prealloc", "runs",
                     "median_req_s", "min_req_s", "max_req_s",
                     "median_run_p50_ms", "median_run_p99_ms"])
    for key, rows in sorted(groups.items()):
        rates = [r["req_s"] for r in rows]
        writer.writerow([*key, len(rows), statistics.median(rates), min(rates),
                         max(rates), statistics.median(r["p50_ms"] for r in rows),
                         statistics.median(r["p99_ms"] for r in rows)])


if __name__ == "__main__":
    main()
