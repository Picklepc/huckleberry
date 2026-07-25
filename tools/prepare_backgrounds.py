#!/usr/bin/env python3
"""Normalize Huckleberry display backgrounds to 320x240 JPEG assets.

Source images live in bg/. This writes normalized JPEGs back to bg/ when a
matching JPEG does not already exist, and mirrors every normalized asset into
data/bg/ for PlatformIO SPIFFS uploads.
"""

from __future__ import annotations

import shutil
from pathlib import Path

from PIL import Image, ImageOps


ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = ROOT / "bg"
SPIFFS_DIR = ROOT / "data" / "bg"
SIZE = (320, 240)
QUALITY = 86


def background_fill(img: Image.Image) -> tuple[int, int, int]:
    rgb = img.convert("RGB")
    w, h = rgb.size
    pts = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    vals = [rgb.getpixel(p) for p in pts]
    return tuple(sum(v[i] for v in vals) // len(vals) for i in range(3))


def normalize(src: Path, dest: Path) -> None:
    with Image.open(src) as raw:
        img = ImageOps.exif_transpose(raw)
        if img.mode in ("RGBA", "LA"):
            fill = background_fill(img)
            flat = Image.new("RGB", img.size, fill)
            flat.paste(img, mask=img.getchannel("A"))
            img = flat
        else:
            img = img.convert("RGB")

        fill = background_fill(img)
        fitted = ImageOps.contain(img, SIZE, Image.Resampling.LANCZOS)
        out = Image.new("RGB", SIZE, fill)
        out.paste(fitted, ((SIZE[0] - fitted.width) // 2, (SIZE[1] - fitted.height) // 2))
        dest.parent.mkdir(parents=True, exist_ok=True)
        out.save(dest, "JPEG", quality=QUALITY, optimize=True, progressive=False)


def source_for_stem(stem: str) -> Path:
    jpg = SRC_DIR / f"{stem}.jpg"
    if jpg.exists():
        return jpg
    jpeg = SRC_DIR / f"{stem}.jpeg"
    if jpeg.exists():
        return jpeg
    return SRC_DIR / f"{stem}.png"


def main() -> int:
    if not SRC_DIR.exists():
        raise SystemExit("bg/ does not exist")

    stems = sorted({
        p.stem
        for p in SRC_DIR.iterdir()
        if p.is_file() and p.suffix.lower() in {".png", ".jpg", ".jpeg"}
    })

    if not stems:
        raise SystemExit("no PNG/JPEG backgrounds found in bg/")

    for stem in stems:
        src = source_for_stem(stem)
        bg_jpg = SRC_DIR / f"{stem}.jpg"
        spiffs_jpg = SPIFFS_DIR / f"{stem}.jpg"

        if bg_jpg.exists() and src == bg_jpg:
            with Image.open(bg_jpg) as img:
                exact_jpeg = img.format == "JPEG" and img.size == SIZE
            if not exact_jpeg:
                normalize(bg_jpg, bg_jpg)
        else:
            normalize(src, bg_jpg)

        SPIFFS_DIR.mkdir(parents=True, exist_ok=True)
        shutil.copy2(bg_jpg, spiffs_jpg)
        print(f"{src.name} -> {bg_jpg.relative_to(ROOT)} -> {spiffs_jpg.relative_to(ROOT)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
