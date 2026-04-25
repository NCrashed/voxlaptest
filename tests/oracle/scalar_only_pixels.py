#!/usr/bin/env python3
"""For each scene, list the SCALAR-ONLY pixels (where asm == fallback != scalar)
— these are the bug pixels we need to explain. Show coords + RGBA triple."""

import sys
from pathlib import Path
from PIL import Image

SCENES = ["diag_down", "high_down", "sprite_front", "sprite_above", "sprite_iso"]


def load(d, name):
    return Image.open(Path(d) / f"{name}.png").convert("RGBA").load()


def main():
    root = Path.cwd()
    asm = load(root / "oracle-windows-msvc-x86", sys.argv[1])
    sca = load(root / "oracle-windows-msvc-x86-scalar-grouscan", sys.argv[1])
    fal = load(root / "oracle-windows-msvc-x86-usev5asm-fallback", sys.argv[1])
    W, H = 640, 480
    print(f"# {sys.argv[1]}: scalar-only pixels (asm==fallback != scalar)")
    print(f"#   x  y    asm                 scalar              fallback")
    n = 0
    for y in range(H):
        for x in range(W):
            a, c, f = asm[x, y], sca[x, y], fal[x, y]
            if a == f and a != c:
                if n < 80:
                    print(f"  {x:>3} {y:>3}   {str(a):<20} {str(c):<20} {str(f):<20}")
                n += 1
    print(f"# total: {n}")


if __name__ == "__main__":
    main()
