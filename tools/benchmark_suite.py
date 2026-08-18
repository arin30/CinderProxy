#!/usr/bin/env python3
import os
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = int(os.environ.get("BENCH_RUNS", "5"))


def parse_metric(output, name):
    prefix = f"{name}:"
    for line in output.splitlines():
        if line.startswith(prefix):
            return float(line.split(":", 1)[1].strip())
    raise ValueError(f"missing metric: {name}")


def main():
    throughputs = []
    p95s = []
    failures = []

    for i in range(RUNS):
        proc = subprocess.run(
            [sys.executable, "tools/benchmark.py"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=True,
            env=os.environ.copy(),
        )
        rps = parse_metric(proc.stdout, "requests_sec")
        p95 = parse_metric(proc.stdout, "latency_p95_ms")
        failed = int(parse_metric(proc.stdout, "failed"))
        throughputs.append(rps)
        p95s.append(p95)
        failures.append(failed)
        print(f"run {i + 1}: {rps:.1f} req/s, {p95:.3f} ms p95, {failed} failed")

    print("\nCinderProxy benchmark summary")
    print(f"runs:                  {RUNS}")
    print(f"median_requests_sec:   {statistics.median(throughputs):.1f}")
    print(f"min_requests_sec:      {min(throughputs):.1f}")
    print(f"max_requests_sec:      {max(throughputs):.1f}")
    print(f"median_latency_p95_ms: {statistics.median(p95s):.3f}")
    print(f"max_latency_p95_ms:    {max(p95s):.3f}")
    print(f"total_failed:          {sum(failures)}")


if __name__ == "__main__":
    main()
