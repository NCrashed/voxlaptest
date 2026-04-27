#!/usr/bin/env python3
"""Compare gline-entry (E) events between two grouscan trace logs.

H4 falsifier: the entry trace logs the freshly-computed gline init
values (cx0/cy0/cx1/cy1/gpz/gdz/gxmax/gi0/gi1). Both asm and scalar
builds run the same C init code, so init values should be bit-
identical at every call. If this script finds a divergence at
some call N, it confirms H4 (compile-time float-rounding cascades).

Usage:
    trace_init_diff.py LOG_A LOG_B
"""

import re
import sys

# Match all E-record fields except the v=%p (process address, expected to differ).
# Capture call number, leng, gstartz0, gstartz1, cx0, cy0, cx1, cy1, gpz0, gpz1,
# gdz0, gdz1, gxmax, gi0, gi1.
RE = re.compile(
    r"^\[c=(?P<c>\d+)\] E "
    r"leng=(?P<leng>\d+) "
    r"v=[A-Fa-f0-9]+ "
    r"z0=(?P<z0>-?\d+) z1=(?P<z1>-?\d+) "
    r"cx0=(?P<cx0>[A-Fa-f0-9]+) cy0=(?P<cy0>[A-Fa-f0-9]+) "
    r"cx1=(?P<cx1>[A-Fa-f0-9]+) cy1=(?P<cy1>[A-Fa-f0-9]+) "
    r"gpz0=(?P<gpz0>[A-Fa-f0-9]+) gpz1=(?P<gpz1>[A-Fa-f0-9]+) "
    r"gdz0=(?P<gdz0>[A-Fa-f0-9]+) gdz1=(?P<gdz1>[A-Fa-f0-9]+) "
    r"gxmax=(?P<gxmax>[A-Fa-f0-9]+) "
    r"gi0=(?P<gi0>[A-Fa-f0-9]+) gi1=(?P<gi1>[A-Fa-f0-9]+)"
)


def parse(path):
    out = {}
    with open(path) as f:
        for line in f:
            m = RE.match(line)
            if not m:
                continue
            d = m.groupdict()
            c = int(d.pop("c"))
            out[c] = d
    return out


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} LOG_A LOG_B", file=sys.stderr)
        sys.exit(2)
    a = parse(sys.argv[1])
    b = parse(sys.argv[2])

    keys = sorted(set(a) | set(b))
    print(f"# {sys.argv[1]} has {len(a)} E records")
    print(f"# {sys.argv[2]} has {len(b)} E records")

    n_match = 0
    n_diff = 0
    n_only_a = 0
    n_only_b = 0
    first_diff = None
    diff_field_counts = {}

    for c in keys:
        if c not in a:
            n_only_b += 1
            continue
        if c not in b:
            n_only_a += 1
            continue
        if a[c] == b[c]:
            n_match += 1
            continue
        n_diff += 1
        for k in a[c]:
            if a[c][k] != b[c][k]:
                diff_field_counts[k] = diff_field_counts.get(k, 0) + 1
        if first_diff is None:
            first_diff = (c, a[c], b[c])

    print(f"\n# summary: match={n_match} diff={n_diff} "
          f"only_A={n_only_a} only_B={n_only_b}")

    if n_diff:
        print(f"\n# fields that ever differed (count of calls):")
        for k, n in sorted(diff_field_counts.items(), key=lambda kv: -kv[1]):
            print(f"  {k:8s} {n}")

        c, da, db = first_diff
        print(f"\n# first divergent call: c={c}")
        for k in sorted(da):
            mark = " *" if da[k] != db[k] else "  "
            print(f"  {mark} {k:8s} A={da[k]:>10s}  B={db[k]:>10s}")
    else:
        print("\n# H4 FALSIFIED: all E records bit-identical between the two builds.")


if __name__ == "__main__":
    main()
