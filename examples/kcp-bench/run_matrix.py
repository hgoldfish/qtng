#!/usr/bin/env python3
"""Run kcp-bench matrices, aggregate them with median/MAD, and compare baselines.

The benchmark binary emits one JSON object per sampled interval plus a final
"summary" object per run (see bench_stats.h / bench_main.cpp). This driver runs
each configuration several times, reduces the summary metrics to median + MAD,
and (optionally) diffs them against a stored baseline to flag regressions.

Examples:
    ./run_matrix.py --suite quick
    ./run_matrix.py --suite phase1 --samples 3 --out baseline/baseline.json
    ./run_matrix.py --suite ablation --samples 5 --base baseline/baseline.json
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))

# Metrics reduced per configuration. Higher is better unless the name ends in _us,
# _resends or _dropped (those are reported for attribution, not gated).
THROUGHPUT_METRIC = "goodput_mibps"
LATENCY_METRIC = "probe_p99_us"
REGRESSION_METRIC = "goodput_mibps"


def base_config(mode, direction="up", **kwargs):
    cfg = {
        "link": "sim",
        "mode": mode,
        "direction": direction,
        "seconds": 2,
        "warmup": 1,
        "mtu": 1400,
        "version": 2,
        "tuner": "on",
    }
    cfg.update(kwargs)
    return cfg


def suite_quick():
    """Small, fast set for smoke tests and CI."""
    return [
        base_config("throughput", rtt=0, seconds=2),
        base_config("throughput", rtt=80, seconds=2),
        base_config("throughput", rtt=80, loss=0.01, seconds=2),
        base_config("latency", rtt=20, count=50, frame_bytes=64),
        base_config("latency", rtt=80, count=50, frame_bytes=64),
        base_config("probe", rtt=20, rate="512K", seconds=2),
    ]


def suite_phase1():
    """Plan Phase 1: throughput over RTT x loss x direction, plus latency."""
    configs = []
    for rtt in (0, 80):
        for loss in (0.0, 0.01):
            for direction in ("up", "down", "bidi"):
                configs.append(base_config("throughput", direction=direction, rtt=rtt, loss=loss, seconds=2))
    for rtt in (20, 80):
        for loss in (0.0, 0.01):
            configs.append(base_config("latency", rtt=rtt, loss=loss, count=50, frame_bytes=64))
    return configs


def suite_ablation():
    """Suite B: flip one knob at a time on a condition where it matters."""
    # RTT 80ms + 1% loss gives the tuner, fastresend and version knobs something to do.
    common = dict(rtt=80, loss=0.01, seconds=2)
    configs = [
        base_config("throughput", **common),
        base_config("throughput", version=1, **common),
        base_config("throughput", tuner="off", **common),
        base_config("throughput", mtu=9000, **common),
        base_config("throughput", mtu=60000, **common),
        base_config("throughput", direction="down", **common),
        base_config("throughput", direction="bidi", **common),
        base_config("throughput", conns=8, **common),
    ]
    return configs


def suite_wifi():
    """High-bandwidth, random-loss path (a WiFi-shaped link), tuner on vs off.

    The point is that loss on a fat, low-RTT path must not collapse throughput:
    a fixed window (tuner off) is the control, because it has no feedback loop
    between the window and the rate-derived BDP estimate.
    """
    configs = []
    for loss in (0.0, 0.01, 0.05, 0.10, 0.20):
        for tuner in ("on", "off"):
            configs.append(base_config("throughput", rtt=30, rate="12M", loss=loss,
                                       tuner=tuner, seconds=10))
    return configs


SUITES = {
    "quick": suite_quick,
    "phase1": suite_phase1,
    "ablation": suite_ablation,
    "wifi": suite_wifi,
}


def arg_name(key):
    return "--" + key.replace("_", "-")


def config_argv(cfg, binary):
    argv = [binary]
    for key in sorted(cfg):
        argv.append(arg_name(key))
        argv.append(str(cfg[key]))
    return argv


def config_key(cfg):
    # Only effective parameters belong in the key: it is what baseline comparison
    # matches on, so a parameter that changes the measurement without changing
    # the key makes two different runs compare as if they were the same. That is
    # why --seconds and --link are written into cfg before the run rather than
    # substituted at argv build time.
    return ",".join("%s=%s" % (k, cfg[k]) for k in sorted(cfg))


def _temp_jsonl():
    fd, path = tempfile.mkstemp(prefix="kcp_bench_", suffix=".jsonl")
    os.close(fd)
    return path


def run_once(binary, cfg, samples, keep_dir, tag):
    """Run one configuration `samples` times; return the list of summary dicts."""
    summaries = []
    for i in range(samples):
        argv = config_argv(cfg, binary)
        if keep_dir:
            os.makedirs(keep_dir, exist_ok=True)
            jsonl = os.path.join(keep_dir, "%s-%d.jsonl" % (tag, i))
            # The binary appends to --json, so a file left over from an earlier
            # invocation of this suite would make a failed run report the
            # previous run's summary. Start from nothing.
            try:
                os.unlink(jsonl)
            except OSError:
                pass
        else:
            jsonl = _temp_jsonl()
        argv += ["--json", jsonl]
        proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=900)
        summary = None
        if os.path.exists(jsonl):
            with open(jsonl, "r") as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if obj.get("type") == "summary":
                        summary = obj
            if not keep_dir:
                try:
                    os.unlink(jsonl)
                except OSError:
                    pass
        if summary is None:
            # A run that produced no JSON summary is a failure the caller must see.
            summary = {
                "type": "summary",
                "result": "fail",
                "error": "no JSON summary (exit %d): %s"
                % (proc.returncode, proc.stderr.decode("utf-8", "replace").strip()[:200]),
            }
        summaries.append(summary)
    return summaries


def reduce_metric(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    med = statistics.median(values)
    mad = statistics.median([abs(v - med) for v in values]) if len(values) > 1 else 0.0
    return {"median": med, "mad": mad, "n": len(values), "min": min(values), "max": max(values)}


def aggregate(cfg, summaries):
    keys = [
        "goodput_mibps",
        "probe_p99_us",
        "probe_p50_us",
        "cpu_sec",
        "rto_resends",
        "fast_resends",
        "loss_rate",
        "packets_dropped",
        "delivered_bytes",
    ]
    metrics = {}
    for key in keys:
        metrics[key] = reduce_metric([s.get(key) for s in summaries])
    ok = all(s.get("result") == "ok" for s in summaries)
    return {
        "config": cfg,
        "key": config_key(cfg),
        "ok": ok,
        "metrics": metrics,
        "errors": [s.get("error") for s in summaries if s.get("error")],
    }


def fmt(metric):
    if not metric:
        return "-"
    # A single sample has no spread to report. Printing ±0.000 claims a precision
    # the run never measured -- and this file's own caveat says lossy configs vary
    # by 20-90% between runs, so a reader must not see a zero here. Omit the ±.
    if metric.get("n", 0) < 2:
        if metric["median"] >= 1000:
            return "%.0f" % metric["median"]
        return "%.3f" % metric["median"]
    if metric["median"] >= 1000:
        return "%.0f±%.0f" % (metric["median"], metric["mad"])
    return "%.3f±%.3f" % (metric["median"], metric["mad"])


def print_table(results):
    # The key is printed in full: configs are often distinguished only by their
    # last field (tuner=on vs tuner=off is exactly the wifi suite's comparison),
    # so truncating it makes two different rows look identical -- the one thing
    # this table exists to prevent. A long line is recoverable; a hidden
    # difference is not. The numeric columns are wide enough for their own
    # headings, and the separator is measured off the header rather than
    # computed from a magic constant, so the two cannot drift apart.
    width = max([len("config")] + [len(r["key"]) for r in results])
    header = "%-*s %13s %13s %10s %10s" % (width, "config", "goodput MiB/s", "p99 us", "rtoResend", "cpu s")
    print(header)
    print("-" * len(header))
    for r in results:
        m = r["metrics"]
        mark = "" if r["ok"] else "  <-- FAIL"
        print("%-*s %13s %13s %10s %10s%s" % (
            width,
            r["key"],
            fmt(m.get("goodput_mibps")),
            fmt(m.get("probe_p99_us")),
            fmt(m.get("rto_resends")),
            fmt(m.get("cpu_sec")),
            mark,
        ))
    if any(m.get("n", 0) < 2 for r in results for m in r["metrics"].values() if m):
        print("(rows without ± are single-sample; --samples >= 2 reports spread)")


def print_failures(results):
    """Print why each failed config failed. The table can only say FAIL."""
    for r in results:
        if r["ok"] or not r.get("errors"):
            continue
        for err in r["errors"]:
            print("  FAIL %s: %s" % (r["key"], err))


def compare_with_base(results, base):
    """Return list of (key, metric, base_median, new_median, delta_pct, worse, ran_ok).

    ran_ok is the config's own ok flag. A delta computed from a partial set of
    samples (some runs produced no summary) is not attributable to the change
    being measured, so the caller must not print a verdict for it.
    """
    regressions = []
    base_by_key = {entry["key"]: entry for entry in base.get("results", [])}
    for r in results:
        b = base_by_key.get(r["key"])
        if not b:
            continue
        # Pick the metric that this mode actually reports: latency runs have
        # goodput_mibps == 0.0, so "goodput if present" would silently skip them.
        mode = r.get("config", {}).get("mode", "")
        metric = LATENCY_METRIC if mode == "latency" else REGRESSION_METRIC
        new_m = r["metrics"].get(metric)
        old_m = b.get("metrics", {}).get(metric)
        if not new_m or not old_m or old_m.get("median") in (None, 0):
            continue
        delta = (new_m["median"] - old_m["median"]) / old_m["median"] * 100.0
        # For latency (_us) a rise is a regression; for goodput a drop is.
        worse = delta > 0 if metric.endswith("_us") else delta < 0
        regressions.append((r["key"], metric, old_m["median"], new_m["median"], delta, worse, r["ok"]))
    return regressions


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=os.path.join(HERE, "build", "kcp-bench"))
    ap.add_argument("--suite", choices=sorted(SUITES), default="quick")
    ap.add_argument("--samples", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=None,
                    help="override the measure window for every config (folds into the config key; "
                         "latency mode is driven by --count and ignores it)")
    ap.add_argument("--out", default=None, help="write aggregated JSON here")
    ap.add_argument("--base", default=None, help="compare against this baseline JSON")
    ap.add_argument("--keep-jsonl", default=None, help="directory to keep per-sample JSONL")
    ap.add_argument("--link", choices=["sim", "real"], default=None,
                    help="override the link for every config (real strips sim-only fault args)")
    ap.add_argument("--threshold", type=float, default=3.0, help="regression threshold in percent")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print("binary not found: %s" % args.binary, file=sys.stderr)
        print("build it first: cmake -S . -B build && cmake --build build -j", file=sys.stderr)
        return 2

    if args.samples < 1:
        # Zero samples produces no summaries, which aggregate() reports as ok with
        # every metric None: a passing suite that measured nothing, and a --out
        # file that later compares as "no comparable configs" while exiting 0.
        print("--samples must be at least 1 (got %d)" % args.samples, file=sys.stderr)
        return 2

    configs = SUITES[args.suite]()

    # Both overrides are folded into each config before anything runs, so argv,
    # the config key and the --out record all agree on what was measured.
    for cfg in configs:
        if args.seconds is not None:
            cfg["seconds"] = args.seconds
        if args.link:
            cfg["link"] = args.link
            if args.link == "real":
                for key in ("rtt", "loss", "reorder", "reorder_extra", "dup", "corrupt", "rate", "seed"):
                    cfg.pop(key, None)

    if args.dry_run:
        for cfg in configs:
            print(" ".join(config_argv(cfg, args.binary)))
        return 0

    results = []
    for idx, cfg in enumerate(configs, 1):
        tag = "cfg%03d" % idx
        print("[%d/%d] %s" % (idx, len(configs), config_key(cfg)), file=sys.stderr)
        summaries = run_once(args.binary, cfg, args.samples, args.keep_jsonl, tag)
        results.append(aggregate(cfg, summaries))

    print()
    print_table(results)
    print_failures(results)

    if args.base:
        with open(args.base, "r") as fh:
            base = json.load(fh)
        regressions = compare_with_base(results, base)
        print()
        if regressions:
            print("vs baseline (%s):" % args.base)
            for key, metric, old, new, delta, worse, ran_ok in regressions:
                # Full key, for the same reason as print_table: a regression
                # report that cannot name the config is not actionable.
                if not ran_ok:
                    # Some samples produced no summary, so the delta is over a
                    # partial set and cannot be attributed to this change.
                    print("  %-8s %-14s %10.3f -> %10.3f (%+.1f%%)  %s  (RUN FAILED)"
                          % ("SKIP", metric, old, new, delta, key))
                    continue
                flag = "REGRESSION" if (worse and abs(delta) > args.threshold) else "ok"
                print("  %-8s %-14s %10.3f -> %10.3f (%+.1f%%)  %s"
                      % (flag, metric, old, new, delta, key))
        else:
            print("vs baseline (%s): no comparable configs" % args.base)

    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        payload = {
            "suite": args.suite,
            "samples": args.samples,
            "generated": datetime.now(timezone.utc).isoformat(),
            "results": results,
        }
        with open(args.out, "w") as fh:
            json.dump(payload, fh, indent=2, sort_keys=True)
        print("\nwrote %s" % args.out)

    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
