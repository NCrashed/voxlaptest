#!/usr/bin/env python3
"""Diff two grouscan trace logs, focusing on per-pixel writes.

Reads two logs (typical pair: scalar vs C-fallback for high_down) and:
  1. Reports the FIRST line where the two diverge.
  2. Filters to write events (lines starting with "[c=N] W..."), groups them
     by (call, slot), and reports any write whose (label, color) differs.
  3. Optionally restricts to a subset of (call, slot) pairs given on stdin
     (one "C SLOT" per line) — useful once we know which slots own the
     43 high_down bug pixels.

Usage:
    trace_diff.py SCALAR.log FALLBACK.log
    trace_diff.py SCALAR.log FALLBACK.log --slots slot_list.txt
"""

import argparse
import re
import sys
from collections import defaultdict


WRITE_RE = re.compile(
    r"^\[c=(?P<c>\d+)\]\s+W(?P<phase>\w+)\s+slot=(?P<slot>-?\d+)\s+col=(?P<col>[0-9a-fA-F]+)"
)


def parse_writes(path):
    """Return a dict {(call, slot): [(line_no, phase, col), ...]}."""
    out = defaultdict(list)
    with open(path) as f:
        for n, line in enumerate(f, 1):
            m = WRITE_RE.match(line)
            if not m:
                continue
            key = (int(m["c"]), int(m["slot"]))
            out[key].append((n, m["phase"], m["col"].lower()))
    return out


def first_divergent_line(path_a, path_b):
    """Stream-compare line-by-line. Returns the first line index where they
    differ (1-based), or None if identical / one is a prefix of the other."""
    with open(path_a) as fa, open(path_b) as fb:
        for n, (la, lb) in enumerate(zip(fa, fb), 1):
            if la != lb:
                return n, la.rstrip(), lb.rstrip()
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a", help="trace log A (typically scalar)")
    ap.add_argument("b", help="trace log B (typically C-fallback)")
    ap.add_argument("--slots", help="optional file of 'call slot' pairs to focus on")
    args = ap.parse_args()

    fdl = first_divergent_line(args.a, args.b)
    if fdl:
        n, la, lb = fdl
        print(f"# first divergent line: {n}")
        print(f"  A: {la}")
        print(f"  B: {lb}")
    else:
        print("# logs identical line-by-line (or one is a prefix of the other)")

    a = parse_writes(args.a)
    b = parse_writes(args.b)

    keys = set(a) | set(b)
    if args.slots:
        focus = set()
        with open(args.slots) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                c, s = line.split()
                focus.add((int(c), int(s)))
        keys &= focus
        print(f"# focused on {len(focus)} (call,slot) pairs; {len(keys)} present")

    print(f"\n# write-event differences (per call,slot)")
    print(f"#   {'call':>5} {'slot':>5}  A_phase A_col          B_phase B_col          status")
    n_diff = 0
    n_same = 0
    n_only_a = 0
    n_only_b = 0
    for k in sorted(keys):
        ea = a.get(k, [])
        eb = b.get(k, [])
        # Keep the LAST write at each (call,slot) — it's what shows in PNG.
        la = ea[-1] if ea else None
        lb = eb[-1] if eb else None
        if la and lb:
            if la[1:] == lb[1:]:
                n_same += 1
                continue
            status = "DIFF"
            n_diff += 1
        elif la and not lb:
            status = "A_ONLY"
            n_only_a += 1
        else:
            status = "B_ONLY"
            n_only_b += 1
        ap_phase = la[1] if la else "-"
        ap_col   = la[2] if la else "-"
        bp_phase = lb[1] if lb else "-"
        bp_col   = lb[2] if lb else "-"
        print(f"  {k[0]:>5} {k[1]:>5}  {ap_phase:<7} {ap_col:<14} {bp_phase:<7} {bp_col:<14} {status}")
        n_diff += (status == "DIFF")
    print(f"\n# summary: same={n_same} diff={n_diff} only_A={n_only_a} only_B={n_only_b}")


if __name__ == "__main__":
    main()
