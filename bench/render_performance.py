#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Render recorded run medians and observed ranges; no estimated datapoints."""
import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ORDER = ["matched_close512", "legacy_optimized4", "hardened4"]
LABELS = ["Legacy close\nbacklog 512",
          "Recovered keep-alive\n4 listeners", "Hardened keep-alive\n4 listeners"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = json.loads(args.summary.read_text())
    args.output.mkdir(parents=True, exist_ok=True)
    values = [data["profiles"][name] for name in ORDER]
    plt.rcParams.update({"font.family": "DejaVu Sans", "svg.hashsalt": "khttpd",
                         "axes.spines.top": False, "axes.spines.right": False})
    fig, axes = plt.subplots(1, 2, figsize=(12.4, 5.4), layout="constrained")
    x = np.arange(len(ORDER))

    def series(metric, scale=1):
        med = np.array([s[metric]["median"] for s in values]) * scale
        low = med - np.array([s[metric]["minimum"] for s in values]) * scale
        high = np.array([s[metric]["maximum"] for s in values]) * scale - med
        return med, np.stack([low, high])

    med, spread = series("req_s", .001)
    axes[0].bar(x, med, color=["#65758b", "#397ec2", "#138b75"],
                width=.68, yerr=spread, capsize=4)
    for i, v in enumerate(med):
        axes[0].annotate(f"{v:,.1f}k", (i, v + spread[1, i]), xytext=(0, 7),
                         textcoords="offset points", ha="center", fontsize=10)
    axes[0].set_ylabel("Completed requests / second (thousands)")
    axes[0].set_title("Throughput", loc="left", fontweight="bold")
    axes[0].set_ylim(0, max(med + spread[1]) * 1.2)
    for offset, metric, label, color in [(-.18, "p50_ms", "p50", "#397ec2"),
                                        (.18, "p99_ms", "p99", "#d58035")]:
        med, spread = series(metric)
        axes[1].bar(x + offset, med, width=.32, yerr=spread, capsize=3,
                    label=label, color=color)
    axes[1].set_yscale("log")
    axes[1].set_ylabel("Latency (milliseconds, log scale)")
    axes[1].set_title("Response latency", loc="left", fontweight="bold")
    axes[1].legend(frameon=False)
    for ax in axes:
        ax.set_xticks(x, LABELS, fontsize=9)
        ax.grid(axis="y", alpha=.18)
        ax.set_axisbelow(True)
    fig.suptitle("kHTTPD — measured on the same Linux VM", fontsize=16,
                 fontweight="bold")
    caption = (f"wrk 4 threads / 256 connections · {data['repetitions']} runs × "
               f"{data['duration']} seconds per setting\n"
               "Bars: medians across runs. Whiskers: observed min–max. "
               "Latency: completed requests; socket errors remain in the report.")
    fig.supxlabel(caption, fontsize=9)
    for extension in ("svg", "png"):
        fig.savefig(args.output / ("comparison." + extension), dpi=170,
                    metadata={"Creator": "kHTTPD recorded benchmark renderer"})
    plt.close(fig)


if __name__ == "__main__":
    main()
