#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarize complete, hash-checked matched runs; preserve every repetition."""
import argparse
import csv
import json
from pathlib import Path
import statistics
import sys
from run import parse, sha

TARGETS = dict(req_s=37739.84, p50_ms=2.99, p99_ms=12.06)


def proc_stat(text):
    lines = text.splitlines()
    cpu = list(map(int, next(x for x in lines if x.startswith("cpu ")).split()[1:]))
    ctxt = int(next(x for x in lines if x.startswith("ctxt ")).split()[1])
    return dict(active=sum(cpu[i] for i in (0, 1, 2, 5, 6)),
                steal=cpu[7] if len(cpu) > 7 else 0, ctxt=ctxt)


def summarize(directory):
    directory = Path(directory).resolve()
    m = json.loads((directory / "manifest.json").read_text())
    if m.get("schema") != 1 or not m.get("complete"):
        raise ValueError("incomplete experiment")
    expected = {(name, rep) for name in m["profiles"]
                for rep in range(m["args"]["repetitions"])}
    seen = set()
    groups = {}
    rows = []
    for r in m["runs"]:
        key = (r["profile"], r["repetition"])
        if key in seen or not r.get("complete") or r.get("returncode") != 0:
            raise ValueError("duplicate or failed repetition")
        seen.add(key)
        for filename, hashname in [
            ("stdout_file", "stdout_sha256"), ("stderr_file", "stderr_sha256"),
            ("validation_file", "validation_sha256"),
        ]:
            path = (directory / r[filename]).resolve()
            if path.parent != directory or sha(path) != r[hashname]:
                raise ValueError("artifact path/checksum mismatch")
        metrics = parse((directory / r["stdout_file"]).read_text(), allow_errors=True)
        parse((directory / r["validation_file"]).read_text(), validated=True, allow_errors=True)
        if r["configuration"] != m["profiles"][r["profile"]]:
            raise ValueError("configuration mismatch")
        before, after = [proc_stat(r[point]["proc_stat"]) for point in ("before", "after")]
        counts = {k: r["after"]["module"].get(k, 0) - v
                  for k, v in r["before"]["module"].items()}
        row = dict(profile=r["profile"], repetition=r["repetition"], **metrics,
            system_context_switches_per_request=(after["ctxt"]-before["ctxt"])/metrics["requests"],
            system_active_cpu_s=(after["active"]-before["active"])/m["environment"]["hz"],
            steal_cpu_s=(after["steal"]-before["steal"])/m["environment"]["hz"],
            module_delta=counts)
        if counts.get("protocol_errors", 0) or counts.get("alloc_errors", 0):
            raise ValueError("hardened protocol/allocation errors")
        rows.append(row)
        groups.setdefault(r["profile"], []).append(row)
    if seen != expected:
        raise ValueError("missing or unexpected experiment cell")
    result = dict(source_commit=m["source_commit"], environment=m["environment"],
                  repetitions=m["args"]["repetitions"], duration=m["args"]["duration"],
                  profiles={}, paired_comparisons={}, runs=rows)
    for name, values in groups.items():
        stats = {"runs": len(values)}
        for metric in ("req_s", "avg_ms", "p50_ms", "p99_ms", "socket_error_events",
                       "system_context_switches_per_request", "system_active_cpu_s"):
            numbers = [r[metric] for r in values]
            stats[metric] = dict(median=statistics.median(numbers),
                                 minimum=min(numbers), maximum=max(numbers))
        stats["all_runs_above_historical_throughput"] = min(r["req_s"] for r in values) >= TARGETS["req_s"]
        stats["all_runs_below_historical_p50"] = max(r["p50_ms"] for r in values) <= TARGETS["p50_ms"]
        stats["all_runs_below_historical_p99"] = max(r["p99_ms"] for r in values) <= TARGETS["p99_ms"]
        result["profiles"][name] = stats
    if "matched_close512" in groups:
        base = {r["repetition"]: r for r in groups["matched_close512"]}
        for name in ("legacy_optimized1", "legacy_optimized4", "hardened1", "hardened4"):
            if name not in groups:
                continue
            paired = dict(
                throughput=[r["req_s"]/base[r["repetition"]]["req_s"] for r in groups[name]],
                p50_reduction=[1-r["p50_ms"]/base[r["repetition"]]["p50_ms"] for r in groups[name]],
                p99_reduction=[1-r["p99_ms"]/base[r["repetition"]]["p99_ms"] for r in groups[name]])
            result["paired_comparisons"][name] = {
                key: dict(median=statistics.median(values), minimum=min(values),
                          maximum=max(values), repetitions=values)
                for key, values in paired.items()}
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("directory", type=Path)
    args = p.parse_args()
    result = summarize(args.directory)
    (args.directory / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    with (args.directory / "summary.csv").open("w") as out:
        writer = csv.writer(out)
        writer.writerow(["profile", "n", "median_req_s", "min_req_s", "max_req_s",
                         "median_p50_ms", "median_p99_ms"])
        for name, s in result["profiles"].items():
            writer.writerow([name, s["runs"], s["req_s"]["median"],
                s["req_s"]["minimum"], s["req_s"]["maximum"],
                s["p50_ms"]["median"], s["p99_ms"]["median"]])
    lines = [
        "# Matched performance measurements", "",
        "Source commit: " + result["source_commit"],
        "Kernel: " + result["environment"]["uname"].strip(), "",
        f"Each cell: {result['repetitions']} runs, {result['duration']} seconds, wrk 4T/256C.",
        "Primary measurement is plain wrk; a separate same-load phase validates every response.",
        "Values below are medians across runs, not pooled percentiles.", "",
        "| Profile | req/s | p50 ms | p99 ms | socket error events | min–max req/s |",
        "|---|---:|---:|---:|---:|---:|"]
    for name, s in result["profiles"].items():
        lines.append(f"| {name} | {s['req_s']['median']:.2f} | "
            f"{s['p50_ms']['median']:.3f} | {s['p99_ms']['median']:.3f} | "
            f"{s['socket_error_events']['median']:.0f} | "
            f"{s['req_s']['minimum']:.2f}–{s['req_s']['maximum']:.2f} |")
    lines.extend(["", "Ratios use the matched_close512 control in the same repetition.", ""])
    for name, s in result["paired_comparisons"].items():
        lines.append(f"- {name}: median paired throughput {s['throughput']['median']:.2f}x "
            f"(range {s['throughput']['minimum']:.2f}–{s['throughput']['maximum']:.2f}x); "
            f"p50 reduction {100*s['p50_reduction']['median']:.1f}%; "
            f"p99 reduction {100*s['p99_reduction']['median']:.1f}%.")
    lines.extend(["",
        "historical_close112 preserves the valid 112-byte close/log/clear/single-listener "
        "behavior from source 3fbe63b. It matches the recorded old response-size shape.",
        "matched_close512 raises its backlog to the optimized value to avoid attributing "
        "simple accept-queue capacity to the remaining optimizations. "
        "It is the conservative primary control.",
        "Socket error events are reported, never omitted; latency percentiles are for "
        "completed requests. Server close-phase I/O counters can include peers terminating "
        "in-flight work after wrk stops and are not equivalent to load-generator errors.",
        "These are measurements on the recorded hosted VM. They do not retroactively "
        "establish the exact hardware/configuration of the 2025 logs.", ""])
    (args.directory / "summary.md").write_text("\n".join(lines))
    print("\n".join(lines))
    print("SUMMARY_JSON " + json.dumps({k: v for k, v in result.items()
          if k not in ("runs", "environment")}), flush=True)


if __name__ == "__main__":
    main()
