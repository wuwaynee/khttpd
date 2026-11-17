#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Controlled local microbenchmarks; requires a clean checkout and built binaries."""
import argparse
from datetime import datetime, timezone
import itertools
import json
import os
from pathlib import Path
import random
import shutil
import socket
import subprocess
import time
from summarize import digest, parse_wrk

ROOT = Path(__file__).resolve().parents[1]


def command(argv, check=True):
    return subprocess.run(list(map(str, argv)), text=True, capture_output=True, check=check)


def privileged(argv):
    return argv if os.geteuid() == 0 else ["sudo", "-n", *argv]


def cpu_set(text):
    result = set()
    for part in text.split(","):
        bounds = part.split("-")
        if len(bounds) == 1:
            result.add(int(part))
        elif len(bounds) == 2:
            first, last = map(int, bounds)
            if first < 0 or last < first or last - first > 65536:
                raise ValueError("invalid CPU range")
            result.update(range(first, last + 1))
        else:
            raise ValueError("invalid CPU set")
    if not result:
        raise ValueError("empty CPU set")
    return result


def read(path):
    try:
        return Path(path).read_text()
    except OSError as error:
        return str(error)


def wait_ready(port, process=None):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise RuntimeError("reference server exited")
        try:
            with socket.create_connection(("127.0.0.1", port), 0.2) as sock:
                sock.sendall(b"GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n")
                if b"200 OK" in sock.recv(512):
                    return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def snapshot():
    return {
        "utc": datetime.now(timezone.utc).isoformat(),
        "proc_stat": read("/proc/stat"), "loadavg": read("/proc/loadavg"),
        "net_dev": read("/proc/net/dev"),
        "threads": command(["ps", "-eLo", "pid,tid,psr,comm,pcpu"], False).stdout,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--implementation", choices=["userspace", "kernel", "both"],
                        default="userspace")
    parser.add_argument("--single", action="store_true",
                        help="only keep-alive=1, shards=1, prealloc=1")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--server-cpus", required=True)
    parser.add_argument("--client-cpus", required=True)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--connections", type=int, default=16)
    parser.add_argument("--workers", type=int, default=64)
    parser.add_argument("--shards", type=int, default=4)
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--seed", type=int, default=20261116)
    parser.add_argument("--port", type=int, default=8081)
    args = parser.parse_args()
    os.chdir(ROOT)
    server, client = cpu_set(args.server_cpus), cpu_set(args.client_cpus)
    allowed = os.sched_getaffinity(0)
    if server & client or not (server | client) <= allowed:
        parser.error("CPU sets must be disjoint and available to this process")
    if not (1 <= args.threads <= args.connections <= args.workers <= 512 and
            1 <= args.shards <= min(args.workers, 64) and 1 <= args.port <= 65535 and
            args.duration >= 1 and args.warmup >= 0 and args.repetitions >= 1):
        parser.error("invalid limits")
    for binary in ("wrk", "taskset", "git", "ps"):
        if not shutil.which(binary):
            parser.error("missing executable: " + binary)
    if command(["git", "status", "--porcelain"]).stdout.strip():
        parser.error("use a clean committed checkout")
    if Path("/sys/module/khttpd").exists():
        parser.error("khttpd is already loaded; refusing to manage an existing module")
    # Refuse an occupied port before touching server state.
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(("127.0.0.1", args.port))
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    source = {
        "commit": command(["git", "rev-parse", "HEAD"]).stdout.strip(),
        "tree": command(["git", "rev-parse", "HEAD^{tree}"]).stdout.strip(),
        "clean": True,
    }
    binaries = {}
    impls = ["userspace", "kernel"] if args.implementation == "both" else [args.implementation]
    for name in impls:
        binary = ROOT / ("khttpd.ko" if name == "kernel" else "build/reference_server")
        binaries[name] = {"path": str(binary), "sha256": digest(binary)}
    wrk = Path(shutil.which("wrk")).resolve()
    meta = {
        "schema": 1, "complete": False, "source": source,
        "args": {**vars(args), "output": str(args.output)},
        "binaries": binaries, "wrk_sha256": digest(wrk),
        "wrk_version": command([wrk, "--version"], False).stdout,
        "uname": command(["uname", "-a"]).stdout,
        "compiler": command(["cc", "--version"], False).stdout,
        "lscpu": command(["lscpu"], False).stdout,
        "os_release": read("/etc/os-release"), "meminfo": read("/proc/meminfo"),
        "cmdline": read("/proc/cmdline"), "cpu_state": {
            str(p): read(p) for pattern in (
                "/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor",
                "/sys/devices/system/cpu/vulnerabilities/*",
                "/sys/devices/system/cpu/intel_pstate/no_turbo",
                "/sys/devices/system/cpu/cpufreq/boost",
            ) for p in Path("/").glob(pattern.lstrip("/"))
        },
        "runs": [],
    }
    manifest = args.output / "manifest.json"

    def save():
        manifest.write_text(json.dumps(meta, indent=2) + "\n")

    save()
    options = [(1, 1, 1)] if args.single else list(itertools.product(
        (0, 1), (1, args.shards) if args.shards != 1 else (1,), (0, 1)))
    cells = [{"implementation": impl, "keep_alive": keep, "shards": shard,
              "prealloc": pre} for impl in impls for keep, shard, pre in options]
    rng = random.Random(args.seed)
    try:
        for rep in range(args.repetitions):
            rng.shuffle(cells)
            for config in cells:
                index = len(meta["runs"])
                prefix = f"{index:03d}"
                process = None
                loaded = False
                log = (args.output / (prefix + ".server.txt")).open("w")
                try:
                    common = [args.port, args.workers, config["shards"],
                              config["keep_alive"], config["prealloc"], 5000, 1024]
                    if config["implementation"] == "userspace":
                        process = subprocess.Popen(
                            ["taskset", "-c", args.server_cpus,
                             str(ROOT / "build/reference_server"), *map(str, common)],
                            stdout=log, stderr=subprocess.STDOUT)
                    else:
                        command(privileged(["insmod", str(ROOT / "khttpd.ko"),
                            f"port={args.port}", f"workers={args.workers}",
                            f"shards={config['shards']}", f"keep_alive={config['keep_alive']}",
                            f"prealloc={config['prealloc']}"]))
                        loaded = True
                        tasks = [p.parent.name for p in Path("/proc").glob("[0-9]*/comm")
                                 if read(p).strip() == "khttpd"]
                        if len(tasks) != args.workers:
                            raise RuntimeError("kernel worker census mismatch")
                        for tid in tasks:
                            command(privileged(["taskset", "-pc", args.server_cpus, tid]))
                    wait_ready(args.port, process)
                    wrk_cmd = ["taskset", "-c", args.client_cpus, str(wrk),
                               f"-t{args.threads}", f"-c{args.connections}",
                               "--timeout", "2s", "--latency", "-s",
                               str(ROOT / "bench/validate.lua")]
                    url = f"http://127.0.0.1:{args.port}/"
                    if args.warmup:
                        warm_cmd = [*wrk_cmd, f"-d{args.warmup}s", url]
                        warm = command(warm_cmd, False)
                        (args.output / (prefix + ".warmup.txt")).write_text(
                            warm.stdout + warm.stderr)
                        if warm.returncode:
                            raise RuntimeError("warmup command failed")
                        parse_wrk(warm.stdout)
                    before = snapshot()
                    argv = [*wrk_cmd, f"-d{args.duration}s", url]
                    measured = command(argv, False)
                    after = snapshot()
                    stdout = prefix + ".wrk.txt"
                    stderr = prefix + ".stderr.txt"
                    (args.output / stdout).write_text(measured.stdout)
                    (args.output / stderr).write_text(measured.stderr)
                    record = {
                        "configuration": dict(config), "repetition": rep,
                        "command": argv, "returncode": measured.returncode,
                        "stdout": stdout, "stderr": stderr,
                        "stdout_sha256": digest(args.output / stdout),
                        "stderr_sha256": digest(args.output / stderr),
                        "before": before, "after": after,
                    }
                    meta["runs"].append(record)
                    save()
                    if measured.returncode:
                        raise RuntimeError("wrk failed")
                    metrics = parse_wrk(measured.stdout)
                    print(json.dumps({**config, "rep": rep, **metrics}), flush=True)
                finally:
                    if process is not None:
                        process.terminate()
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                            raise RuntimeError("reference server shutdown timed out")
                    if loaded:
                        log.write(read("/sys/module/khttpd/parameters/stats"))
                        command(privileged(["rmmod", "khttpd"]))
                    log.close()
                time.sleep(1)
        meta["complete"] = True
        save()
    except BaseException as error:
        meta["error"] = str(error)
        save()
        raise


if __name__ == "__main__":
    main()
