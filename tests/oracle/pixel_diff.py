#!/usr/bin/env python3
"""First-divergent-pixel oracle: compare two PNG sets scene-by-scene."""

import sys
from pathlib import Path
from PIL import Image

SCENES = ["north", "east", "diag_down", "high_down", "sprite_front",
          "sprite_above", "sprite_iso"]


def load_rgba(path: Path):
    img = Image.open(path).convert("RGBA")
    return img, img.load()


def first_divergent(asm_path: Path, scalar_path: Path):
    a_img, a_px = load_rgba(asm_path)
    s_img, s_px = load_rgba(scalar_path)
    if a_img.size != s_img.size:
        return ("size_mismatch", a_img.size, s_img.size, None, None, 0)
    w, h = a_img.size
    first = None
    diff_count = 0
    for y in range(h):
        for x in range(w):
            ap = a_px[x, y]
            sp = s_px[x, y]
            if ap != sp:
                diff_count += 1
                if first is None:
                    first = (x, y, ap, sp)
    return (None, (w, h), None, first, None, diff_count)


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    asm_dir = root / "oracle-windows-msvc-x86"
    scalar_dir = root / "oracle-windows-msvc-x86-scalar-grouscan"
    if not asm_dir.is_dir() or not scalar_dir.is_dir():
        print(f"missing oracle dirs under {root}", file=sys.stderr)
        sys.exit(2)

    print(f"{'scene':<14} {'first_xy':<10} {'asm_rgba':<22} {'scalar_rgba':<22} diff_count")
    for scene in SCENES:
        a = asm_dir / f"{scene}.png"
        s = scalar_dir / f"{scene}.png"
        if not a.is_file() or not s.is_file():
            print(f"{scene:<14} MISSING")
            continue
        err, size, sz2, first, _, count = first_divergent(a, s)
        if err == "size_mismatch":
            print(f"{scene:<14} size_mismatch asm={size} scalar={sz2}")
            continue
        if first is None:
            print(f"{scene:<14} {'-':<10} {'-':<22} {'-':<22} 0  size={size}")
        else:
            x, y, ap, sp = first
            print(f"{scene:<14} ({x:>3},{y:>3})  {str(ap):<22} {str(sp):<22} {count}  size={size}")


if __name__ == "__main__":
    main()
