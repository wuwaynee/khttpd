#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure recovered historical behavior and hardened khttpd on one Linux VM."""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
PROFILES = {
    # The oldest source oversent CRLF; the measured historical 112-byte frame did not.
    "ancient_oversend114": dict(kind="legacy", keep_alive=0, log_requests=1,
        clear_buffer=1, shards=1, backlog=100, use_pool=1, extra_crlf=1),
    "historical_close112": dict(kind="legacy", keep_alive=0, log_requests=1,
        clear_buffer=1, shards=1, backlog=100, use_pool=1, extra_crlf=0),
    "matched_close512": dict(kind="legacy", keep_alive=0, log_requests=1,
        clear_buffer=1, shards=1, backlog=512, use_pool=1, extra_crlf=0),
    "close_no_logging": dict(kind="legacy", keep_alive=0, log_requests=0,
        clear_buffer=1, shards=1, backlog=100, use_pool=1, extra_crlf=0),
    "keepalive_with_logging": dict(kind="legacy", keep_alive=1, log_requests=1,
        clear_buffer=1, shards=1, backlog=100, use_pool=1, extra_crlf=0),
    "keepalive_no_logging": dict(kind="legacy", keep_alive=1, log_requests=0,
        clear_buffer=1, shards=1, backlog=100, use_pool=1, extra_crlf=0),
    "keepalive_logged512": dict(kind="legacy", keep_alive=1, log_requests=1,
        clear_buffer=1, shards=1, backlog=512, use_pool=1, extra_crlf=0),
    "keepalive_quiet512": dict(kind="legacy", keep_alive=1, log_requests=0,
        clear_buffer=1, shards=1, backlog=512, use_pool=1, extra_crlf=0),
    "legacy_optimized1": dict(kind="legacy", keep_alive=1, log_requests=0,
        clear_buffer=0, shards=1, backlog=512, use_pool=1, extra_crlf=0),
    "legacy_optimized4": dict(kind="legacy", keep_alive=1, log_requests=0,
        clear_buffer=0, shards=4, backlog=512, use_pool=1, extra_crlf=0),
    "legacy_no_pool": dict(kind="legacy", keep_alive=1, log_requests=0,
        clear_buffer=0, shards=4, backlog=512, use_pool=0, extra_crlf=0),
    "hardened1": dict(kind="hardened", keep_alive=1, prealloc=1, shards=1,
        workers=512, max_requests=1000000, timeout_ms=5000),
    "hardened4": dict(kind="hardened", keep_alive=1, prealloc=1, shards=4,
        workers=512, max_requests=1000000, timeout_ms=5000),
    "hardened_alloc": dict(kind="hardened", keep_alive=1, prealloc=0, shards=4,
        workers=512, max_requests=1000000, timeout_ms=5000),
    "hardened_close": dict(kind="hardened", keep_alive=0, prealloc=1, shards=1,
        workers=512, max_requests=1000000, timeout_ms=5000),
}


def run(argv, check=True, timeout=90):
    return subprocess.run(list(map(str, argv)), capture_output=True, text=True,
                          check=check, timeout=timeout)


def privileged(argv):
    return argv if os.geteuid() == 0 else ["sudo", "-n", *argv]


def read(path):
    try:
        return Path(path).read_text()
    except OSError as error:
        return str(error)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def counters(text):
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", text)}


def parse(text, validated=False, allow_errors=False):
    def find(pattern):
        result = re.search(pattern, text, re.MULTILINE)
        if not result:
            raise ValueError("missing field: " + pattern)
        return result
    unit = {"us": 0.001, "ms": 1.0, "s": 1000.0, "m": 60000.0}
    def latency(label):
        m = find(r"^\s*" + label + r"\s+([0-9.]+)(us|ms|s|m)")
        return float(m[1]) * unit[m[2]]
    requests = int(find(r"^\s*(\d+) requests in ")[1])
    seconds = float(find(r"^\s*\d+ requests in ([0-9.]+)s,")[1])
    qps = float(find(r"^Requests/sec:\s+([0-9.]+)")[1])
    errors = [tuple(map(int, m.groups())) for m in re.finditer(
        r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", text)]
    status = re.search(r"Non-2xx or 3xx responses:\s+(\d+)", text)
    error_counts = [sum(row[i] for row in errors) for i in range(4)]
    if (any(error_counts) and not allow_errors) or (status and int(status[1])):
        raise ValueError("wrk reported errors")
    if requests <= 0 or qps <= 0:
        raise ValueError("empty measurement")
    if validated:
        m = find(r"^KH_VALIDATED checked=(\d+) invalid=(\d+)")
        if int(m[1]) != requests or int(m[2]):
            raise ValueError("status/body validation failure")
    return dict(requests=requests, seconds=seconds, req_s=qps,
                avg_ms=latency("Latency"), p50_ms=latency("50%"),
                p99_ms=latency("99%"),
                socket_errors=dict(zip(("connect", "read", "write", "timeout"), error_counts)),
                socket_error_events=sum(error_counts))


def snap(module):
    return dict(utc=datetime.now(timezone.utc).isoformat(),
                proc_stat=read("/proc/stat"),
                snmp=read("/proc/net/snmp"),
                module=counters(read(f"/sys/module/{module}/parameters/stats")),
                sockstat=read("/proc/net/sockstat"),
                softirqs=read("/proc/softirqs"))


def wait_ready(config):
    for _ in range(200):
        try:
            with socket.create_connection(("127.0.0.1", 8081), .2) as connection:
                connection.sendall(b"GET / HTTP/1.1\r\nHost: a\r\n\r\n")
                data = b""
                while b"\r\n\r\n" not in data:
                    block = connection.recv(512)
                    if not block:
                        raise RuntimeError("truncated response header")
                    data += block
                header, body = data.split(b"\r\n\r\n", 1)
                while len(body) < 12:
                    block = connection.recv(512)
                    if not block:
                        raise RuntimeError("truncated response body")
                    body += block
                if not header.startswith(b"HTTP/1.1 200 OK\r\n"):
                    raise RuntimeError("unexpected response status")
                if b"\r\nContent-Length: 12" not in header or body[:12] != b"Hello World!":
                    raise RuntimeError("unexpected body framing")
                expected = 117 if config["keep_alive"] else 112
                actual = len(header) + 4 + 12
                if actual != expected:
                    raise RuntimeError(f"unexpected frame length {actual}, expected {expected}")
                return dict(frame_bytes=actual, connection_policy=(
                    "keep-alive" if config["keep_alive"] else "close"),
                    observed_extra_bytes=len(body)-12)
        except OSError:
            pass
        time.sleep(.05)
    raise RuntimeError("server not ready")

def unload(module):
    # Legacy connection module refs forbid unloading with live worker code.
    for _ in range(200):
        refs = read(f"/sys/module/{module}/refcnt").strip()
        if module == "khttpd" or refs == "0":
            run(privileged(["rmmod", module]), timeout=30)
            return
        time.sleep(.05)
    raise RuntimeError("live legacy connections still hold module references")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--duration", type=int, default=30)
    p.add_argument("--warmup", type=int, default=5)
    p.add_argument("--repetitions", type=int, default=5)
    p.add_argument("--profiles", default=",".join(PROFILES))
    p.add_argument("--seed", type=int, default=20261117)
    args = p.parse_args()
    os.chdir(ROOT)
    names = args.profiles.split(",")
    if len(names) != len(set(names)) or any(n not in PROFILES for n in names):
        p.error("unknown/duplicate profiles")
    if args.duration < 1 or args.warmup < 0 or args.repetitions < 1:
        p.error("invalid experiment duration")
    if run(["git", "status", "--porcelain"]).stdout.strip():
        p.error("source checkout is dirty")
    for module in ("khttpd", "khttpd_legacy"):
        if Path("/sys/module/" + module).exists():
            p.error("refusing to manage a preexisting module")
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(("127.0.0.1", 8081))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out / "manifest.json").exists():
        p.error("output manifest already exists")
    source = run(["git", "rev-parse", "HEAD"]).stdout.strip()
    wrk = Path(shutil.which("wrk")).resolve()
    manifest = dict(schema=1, complete=False, source_commit=source,
        args={**vars(args), "output": str(out)}, profiles={n: PROFILES[n] for n in names},
        environment=dict(uname=run(["uname", "-a"]).stdout,
            lscpu=run(["lscpu"]).stdout, os_release=read("/etc/os-release"),
            cpuinfo=read("/proc/cpuinfo"), meminfo=read("/proc/meminfo"),
            cmdline=read("/proc/cmdline"), boot_id=read("/proc/sys/kernel/random/boot_id"),
            printk=read("/proc/sys/kernel/printk"),
            ftrace=read("/sys/kernel/tracing/current_tracer"),
            compiler=run(["cc", "--version"]).stdout,
            build_env={k: os.environ.get(k, "") for k in
                       ("CC", "CFLAGS", "LDFLAGS", "KCFLAGS")},
            wrk_version=run([wrk, "--version"], check=False).stdout,
            hz=os.sysconf("SC_CLK_TCK"), affinity=sorted(os.sched_getaffinity(0))),
        binaries=dict(wrk=sha(wrk), hardened=sha("khttpd.ko"),
                      historical=sha("bench/diagnose/legacy/khttpd_legacy.ko")),
        measurement="plain wrk, 4 threads, 256 connections, latency distribution",
        validation="separate 2-second same-load per-response status/body validation",
        runs=[])
    manifest_path = out / "manifest.json"
    def save():
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    save()
    old_console = read("/proc/sys/kernel/printk").split()[0]
    loaded = None
    try:
        # Avoid serial-console rendering dominating the historical pr_info.
        # Ring-buffer logging remains active; this setting is identical for all cells.
        run(privileged(["dmesg", "-n", "4"]))
        rng = random.Random(args.seed)
        for rep in range(args.repetitions):
            order = list(names)
            rng.shuffle(order)
            for name in order:
                config = PROFILES[name]
                kind = config["kind"]
                module = "khttpd_legacy" if kind == "legacy" else "khttpd"
                binary = ("bench/diagnose/legacy/khttpd_legacy.ko"
                          if kind == "legacy" else "khttpd.ko")
                index = len(manifest["runs"])
                prefix = f"{index:03d}-{name}"
                argv = privileged(["insmod", str(ROOT / binary)] + [
                    f"{key}={value}" for key, value in config.items() if key != "kind"])
                print("START " + json.dumps(dict(profile=name, repetition=rep,
                      duration=args.duration)), flush=True)
                record = dict(profile=name, repetition=rep, configuration=config,
                              load_command=argv, complete=False)
                manifest["runs"].append(record)
                save()
                try:
                    run(argv)
                    loaded = module
                    record['response_shape'] = wait_ready(config)
                    validate_cmd = [str(wrk), "-t4", "-c256", "-d2s", "--latency",
                        "--timeout", "2s", "-s", str(ROOT / "bench/validate.lua"),
                        "http://127.0.0.1:8081/"]
                    validation = run(validate_cmd)
                    validation_path = out / (prefix + ".validation.txt")
                    validation_path.write_text(validation.stdout + validation.stderr)
                    record["validation"] = parse(validation.stdout, validated=True, allow_errors=True)
                    record["validation_file"] = validation_path.name
                    record["validation_sha256"] = sha(validation_path)
                    if args.warmup:
                        warm = run([wrk, "-t4", "-c256", f"-d{args.warmup}s", "--latency",
                                    "--timeout", "2s", "http://127.0.0.1:8081/"])
                        (out / (prefix + ".warmup.txt")).write_text(warm.stdout + warm.stderr)
                        parse(warm.stdout, allow_errors=True)
                    time.sleep(.25)
                    record["before"] = snap(module)
                    argv = [str(wrk), "-t4", "-c256", f"-d{args.duration}s",
                            "--latency", "--timeout", "2s", "http://127.0.0.1:8081/"]
                    measured = run(argv, timeout=args.duration + 30)
                    # Wait briefly for connection-close counters to settle.
                    time.sleep(.25)
                    record["after"] = snap(module)
                    raw = out / (prefix + ".wrk.txt")
                    raw.write_text(measured.stdout)
                    err = out / (prefix + ".stderr.txt")
                    err.write_text(measured.stderr)
                    record.update(command=argv, returncode=measured.returncode,
                        stdout_file=raw.name, stdout_sha256=sha(raw),
                        stderr_file=err.name, stderr_sha256=sha(err),
                        metrics=parse(measured.stdout, allow_errors=True), complete=True)
                    save()
                    print("RESULT " + json.dumps(dict(profile=name, repetition=rep,
                          **record["metrics"], before=record["before"]["module"],
                          after=record["after"]["module"])), flush=True)
                finally:
                    if loaded:
                        unload(loaded)
                        loaded = None
                time.sleep(1)
        manifest["complete"] = True
        save()
    except BaseException as error:
        manifest["error"] = str(error)
        save()
        raise
    finally:
        if loaded:
            unload(loaded)
        run(privileged(["dmesg", "-n", old_console]), check=False)
        # A compact log export is readable through the connector even after artifact expiry.
        for path in sorted(out.glob("*.txt")):
            if path.stat().st_size <= 50000:
                print("EVIDENCE_BEGIN " + path.name)
                print(base64.b64encode(path.read_bytes()).decode())
                print("EVIDENCE_END " + path.name)
        print("MANIFEST_BASE64 " + base64.b64encode(manifest_path.read_bytes()).decode())
    print("DIAGNOSIS_COMPLETE", flush=True)


if __name__ == "__main__":
    main()
