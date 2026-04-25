#!/usr/bin/env python3
"""Detailed look at high_down columns 171-185: full y-pattern asm vs scalar."""

from pathlib import Path
from PIL import Image

root = Path.cwd()
asm = Image.open(root / "oracle-windows-msvc-x86" / "high_down.png").convert("RGBA")
sca = Image.open(root / "oracle-windows-msvc-x86-scalar-grouscan" / "high_down.png").convert("RGBA")
ap = asm.load(); sp = sca.load()

# Color shorthand
NAMES = {
    (0, 0, 0, 255): "BLK",
    (135, 206, 235, 255): "SKY",
    (255, 208, 80, 255): "YEL",
    (96, 104, 120, 255): "GRY",
}


def sname(c):
    return NAMES.get(c, f"#{c[0]:02x}{c[1]:02x}{c[2]:02x}")


# Print full mismatch list
print("== Full mismatch list, high_down ==")
for x in range(165, 195):
    for y in range(450, 480):
        if ap[x, y] != sp[x, y]:
            print(f"  x={x:>3} y={y:>3}  asm={sname(ap[x,y]):<8} scalar={sname(sp[x,y]):<8}")

# Print column dumps for x=176 (middle of the bad range, 6 mismatches)
print("\n== Column-by-column y-strip for x=171..181, y=440..480 ==")
print(f"     {'  '.join(f'{x:>3}' for x in range(171, 182))}")
print("    asm | scalar")
for y in range(440, 480):
    asm_row = " ".join(sname(ap[x, y])[:3] for x in range(171, 182))
    sca_row = " ".join(sname(sp[x, y])[:3] for x in range(171, 182))
    flags = "".join("X" if ap[x, y] != sp[x, y] else "." for x in range(171, 182))
    print(f"y={y}  {asm_row}  |  {sca_row}  {flags}")
