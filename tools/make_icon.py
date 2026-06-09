#!/usr/bin/env python3
"""Generate SwitchInfoNX icon.jpg (256x256 JPEG for elf2nro)."""

from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    raise SystemExit("Install Pillow: pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "icon.jpg"
SIZE = 256

# Use generated PNG if present, otherwise draw a simple fallback icon.
SRC = ROOT / "switchinfo_icon.png"
if SRC.exists():
    img = Image.open(SRC).convert("RGB")
    img = img.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
else:
    img = Image.new("RGB", (SIZE, SIZE), (12, 18, 42))
    draw = ImageDraw.Draw(img)
    draw.ellipse([18, 18, 238, 238], fill=(0, 140, 200), outline=(0, 200, 255), width=4)
    draw.rounded_rectangle([48, 58, 208, 178], radius=18, fill=(8, 24, 48), outline=(0, 220, 255), width=3)
    draw.ellipse([108, 78, 148, 118], fill=(255, 255, 255))
    draw.rounded_rectangle([118, 128, 138, 168], radius=6, fill=(255, 255, 255))
    for i, h in enumerate([18, 28, 22, 34]):
        x = 62 + i * 34
        draw.rounded_rectangle([x, 190 - h, x + 22, 190], radius=4, fill=(0, 220, 180))

img.save(OUT, "JPEG", quality=92, optimize=True)
print(f"Wrote {OUT} ({SIZE}x{SIZE})")
