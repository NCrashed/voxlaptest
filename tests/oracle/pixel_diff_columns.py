#!/usr/bin/env python3
"""Per-column divergence map: for each scene and each x, report (y_min, y_max,
count) of mismatched pixels."""

import sys
from pathlib import Path
from PIL import Image

SCENES = ["diag_down", "high_down", "sprite_front", "sprite_above", "sprite_iso"]


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    asm_dir = root / "oracle-windows-msvc-x86"
    scalar_dir = root / "oracle-windows-msvc-x86-scalar-grouscan"

    for scene in SCENES:
        a = Image.open(asm_dir / f"{scene}.png").convert("RGBA")
        s = Image.open(scalar_dir / f"{scene}.png").convert("RGBA")
        ap = a.load(); sp = s.load(); w, h = a.size

        col_stats = {}
        first_color_pairs = {}
        for x in range(w):
            ymin = ymax = None; count = 0
            for y in range(h):
                if ap[x, y] != sp[x, y]:
                    if ymin is None: ymin = y
                    ymax = y
                    count += 1
                    key = (ap[x, y], sp[x, y])
                    first_color_pairs[key] = first_color_pairs.get(key, 0) + 1
            if count:
                col_stats[x] = (ymin, ymax, count)

        print(f"\n=== {scene}: {len(col_stats)} bad columns ===")
        if not col_stats:
            continue
        keys = sorted(col_stats)
        # show up to 12 evenly-spaced bad columns
        step = max(1, len(keys) // 12)
        for i, x in enumerate(keys[::step][:12]):
            ymin, ymax, count = col_stats[x]
            print(f"  x={x:>3}  ymin={ymin:>3}  ymax={ymax:>3}  bad_pixels={count}")
        print(f"  bad_x_range=[{keys[0]},{keys[-1]}]")
        print(f"  color-pair histogram (top 6):")
        for (asm_c, sca_c), cnt in sorted(first_color_pairs.items(),
                                          key=lambda kv: -kv[1])[:6]:
            print(f"    asm={asm_c} scalar={sca_c}  count={cnt}")


if __name__ == "__main__":
    main()
