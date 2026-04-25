#!/usr/bin/env python3
"""3-way pixel diff: asm vs scalar vs C-fallback (USEV5ASM=0).

For each scene, classifies each pixel as one of:
  - all match: asm == scalar == fallback
  - scalar-only: scalar differs, asm == fallback     (the bug — scalar wrong)
  - fallback-only: fallback differs, asm == scalar   (asm-side specific behaviour)
  - all differ: three distinct colors
  - asm-only: asm differs, scalar == fallback
"""

import sys
from pathlib import Path
from PIL import Image

SCENES = ["north", "east", "diag_down", "high_down", "sprite_front",
          "sprite_above", "sprite_iso"]


def load(d, name):
    return Image.open(Path(d) / f"{name}.png").convert("RGBA").load()


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    asm = root / "oracle-windows-msvc-x86"
    sca = root / "oracle-windows-msvc-x86-scalar-grouscan"
    fal = root / "oracle-windows-msvc-x86-usev5asm-fallback"
    if not all(p.is_dir() for p in (asm, sca, fal)):
        print("missing oracle dirs", file=sys.stderr)
        sys.exit(2)

    W, H = 640, 480
    print(f"{'scene':<14} {'all_match':>9} {'scalar_only':>12} "
          f"{'fallback_only':>14} {'asm_only':>9} {'all_differ':>10}")
    for s in SCENES:
        a = load(asm, s); c = load(sca, s); f = load(fal, s)
        all_match = scalar_only = fallback_only = asm_only = all_differ = 0
        for y in range(H):
            for x in range(W):
                ap, cp, fp = a[x, y], c[x, y], f[x, y]
                if ap == cp == fp:
                    all_match += 1
                elif ap == fp != cp:        # scalar wrong, asm == fallback
                    scalar_only += 1
                elif ap == cp != fp:        # fallback wrong (or just different)
                    fallback_only += 1
                elif cp == fp != ap:        # asm odd one out
                    asm_only += 1
                else:
                    all_differ += 1
        print(f"{s:<14} {all_match:>9} {scalar_only:>12} "
              f"{fallback_only:>14} {asm_only:>9} {all_differ:>10}")


if __name__ == "__main__":
    main()
