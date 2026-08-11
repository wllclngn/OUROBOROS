#!/usr/bin/env python3
"""Capture a gapless track transition with montauk and fold it into an answer.

The ROADMAP item names three suspects for the hitch at a track boundary:

  H1  the next track's decoder is not prebuffered in time, so the ring buffer
      drains while the next file opens and decodes
  H2  cold I/O latency opening the next file
  H3  RT or decode-thread scheduling jitter right at the handoff

montauk is the only tracer. This script drives it, then hands the capture to
montauk_analyze for the FULL report set and reads the JSON envelope, because a
capture on disk gets analyzed and never hand-read.

Run it, reproduce one transition, and read the verdict.
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

# Classes that bear on a track boundary. Narrowing matters: an excluded class is
# never reserved in the ring and is not counted as a drop, so one loud class
# cannot drown the one the capture is for.
CLASSES = "sched,io,ntsync,heap,exit"

# Which reports speak to which hypothesis. The analyzer emits the full set; this
# is only the narrowing for the verdict, never a substitute for reading them.
HYPOTHESES = {
    "H1 decoder not prebuffered in time": ["waits", "sched", "dispatch-stall", "heap"],
    "H2 cold I/O opening the next file": ["io", "waits", "fractal"],
    "H3 RT / decode-thread scheduling jitter": ["sched", "dispatch-stall", "locality", "wakers"],
}


def log(level, msg):
    print(f"[{time.strftime('%H:%M:%S')}] [{level:<5}] {msg}", flush=True)


def preflight(pattern):
    problems = []
    for tool in ("montauk", "montauk_analyze"):
        if shutil.which(tool) is None:
            problems.append(f"{tool} is not on PATH")
    if os.geteuid() != 0:
        problems.append("not root; montauk --trace needs privileges for eBPF attach")
    try:
        out = subprocess.run(["pgrep", "-x", pattern], capture_output=True, text=True)
        if out.returncode != 0:
            problems.append(f"no running process named '{pattern}' to trace")
    except FileNotFoundError:
        pass
    return problems


def capture(pattern, out_file, classes, ring_bytes, seconds):
    cmd = ["montauk", "--trace", pattern, "--trace-out", str(out_file),
           "--trace-classes", classes, "--trace-ring-bytes", ring_bytes]
    log("INFO", "starting capture: " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(1.5)
    if proc.poll() is not None:
        log("ERROR", "montauk exited immediately:")
        print(proc.stderr.read())
        return None

    print()
    print("● reproduce now:")
    print("  Play a track and let it run into the NEXT track so the gapless")
    print("  transition happens while this capture is live. Do it two or three")
    print("  times if the hitch is intermittent.")
    if seconds:
        print(f"  Capturing for {seconds}s.")
        time.sleep(seconds)
    else:
        try:
            input("  Press Enter once you have heard the hitch. ")
        except (EOFError, KeyboardInterrupt):
            pass

    log("INFO", "stopping capture")
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=45)
    except subprocess.TimeoutExpired:
        log("WARN", "montauk did not stop within 45s; sending SIGKILL")
        proc.kill()
        proc.wait()
    return out_file if out_file.exists() and out_file.stat().st_size > 0 else None


def analyze(trace, out_dir):
    text_path = out_dir / "reports.txt"
    json_path = out_dir / "reports.json"

    log("INFO", "analyzing (full report set, text)")
    text = subprocess.run(["montauk_analyze", str(trace)], capture_output=True, text=True)
    text_path.write_text(text.stdout + text.stderr)

    log("INFO", "analyzing (full report set, json)")
    js = subprocess.run(["montauk_analyze", str(trace), "--json"], capture_output=True, text=True)
    json_path.write_text(js.stdout)

    envelope = None
    try:
        envelope = json.loads(js.stdout)
    except json.JSONDecodeError as e:
        log("WARN", f"could not parse the JSON envelope: {e}")
    return text_path, json_path, envelope


def collect_reports(node, found):
    """Walk the envelope and index every object that looks like a report."""
    if isinstance(node, dict):
        name = node.get("name")
        if isinstance(name, str) and ("class" in node or "verdict" in node or "rows" in node):
            found.setdefault(name, []).append(node)
        for v in node.values():
            collect_reports(v, found)
    elif isinstance(node, list):
        for v in node:
            collect_reports(v, found)
    return found


def summarize(node, depth=0):
    """Pull the scalars a human would read off a report, without assuming a schema."""
    out = []
    if isinstance(node, dict):
        for k, v in node.items():
            if isinstance(v, (int, float, str)) and k not in ("name",):
                out.append(f"{k}={v}")
            elif depth < 2:
                out.extend(summarize(v, depth + 1))
    elif isinstance(node, list) and depth < 2:
        for v in node[:4]:
            out.extend(summarize(v, depth + 1))
    return out


def verdict(envelope, text_path, json_path):
    print()
    print("● verdict:")
    if envelope is None:
        print("  No JSON envelope to read. The full text report is at")
        print(f"  {text_path} and must be read in full before theorising.")
        return

    found = collect_reports(envelope, {})
    if not found:
        print("  The envelope parsed but carried no recognisable reports. Read")
        print(f"  {json_path} directly; this narrowing is not a substitute for it.")
        return

    for hypothesis, wanted in HYPOTHESES.items():
        print()
        print(f"  {hypothesis}")
        hit = False
        for name in wanted:
            for report in found.get(name, []):
                hit = True
                fields = summarize(report)
                head = ", ".join(fields[:8]) if fields else "(no scalar fields)"
                print(f"    {name}: {head}")
        if not hit:
            print("    no report in the set spoke to this one")

    print()
    print("  reports not consulted above, present in the capture:")
    rest = sorted(set(found) - {n for w in HYPOTHESES.values() for n in w})
    print("    " + (", ".join(rest) if rest else "(none)"))
    print()
    print(f"  full text  {text_path}")
    print(f"  full json  {json_path}")
    print()
    print("  Read the full set before concluding. If none of the three suspects")
    print("  shows anything, the gap is real: montauk sees syscalls and scheduling")
    print("  but not OUROBOROS's own ring-buffer occupancy, and closing that needs")
    print("  an operator-parameterised counter in montauk, not a workaround here.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pattern", default="ouroboros", help="process pattern to trace")
    ap.add_argument("--out-dir", default="/tmp/ouroboros", help="where the capture and reports land")
    ap.add_argument("--classes", default=CLASSES, help="montauk --trace-classes list")
    ap.add_argument("--ring-bytes", default="16M", help="montauk --trace-ring-bytes")
    ap.add_argument("--seconds", type=int, default=0,
                    help="capture for N seconds instead of waiting for Enter")
    ap.add_argument("--analyze-only", metavar="TRACE",
                    help="skip capture and analyze an existing trace")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.analyze_only:
        trace = Path(args.analyze_only)
        if not trace.exists():
            log("ERROR", f"no such trace: {trace}")
            return 1
    else:
        problems = preflight(args.pattern)
        if problems:
            log("ERROR", "preflight failed:")
            for p in problems:
                print(f"  - {p}")
            return 1
        trace = capture(args.pattern, out_dir / "gapless.trace",
                        args.classes, args.ring_bytes, args.seconds)
        if trace is None:
            log("ERROR", "capture produced nothing")
            return 1
        log("INFO", f"capture: {trace} ({trace.stat().st_size} bytes)")

    text_path, json_path, envelope = analyze(trace, out_dir)
    verdict(envelope, text_path, json_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
