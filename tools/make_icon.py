"""Generates the application icon assets from packaging/icon-source.png.

The checked-in source is a 512x512 RGBA image with transparent rounded corners.
Everything the two packages need is derived from it, so the artwork only has to
be replaced in one place:

  packaging/explosionsim.ico      Windows icon resource (16-256 px)
  packaging/explosionsim.png      256 px icon installed to hicolor/256x256
  packaging/explosionsim-512.png  512 px icon installed to hicolor/512x512
  assets/icon.png                 runtime window icon (SDL_SetWindowIcon)

Run from the repository root:

    python tools/make_icon.py
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "packaging" / "icon-source.png"

# Windows picks the closest entry for the requested DPI; 256 px is stored PNG
# compressed, the smaller sizes as DIBs so old shells can read them.
ICO_SIZES = [
    (16, 16),
    (20, 20),
    (24, 24),
    (32, 32),
    (40, 40),
    (48, 48),
    (64, 64),
    (96, 96),
    (128, 128),
    (256, 256),
]


def resized(source: Image.Image, size: int) -> Image.Image:
    return source.resize((size, size), Image.Resampling.LANCZOS)


def write_png(source: Image.Image, path: Path, size: int) -> None:
    resized(source, size).save(path, format="PNG", optimize=True)
    print(f"wrote {path.relative_to(ROOT)} ({size}x{size})")


def main() -> None:
    source = Image.open(SOURCE).convert("RGBA")
    if source.width != source.height:
        raise SystemExit(f"{SOURCE} must be square, got {source.size}")

    ico = resized(source, 256)
    ico.save(ROOT / "packaging" / "explosionsim.ico", format="ICO",
             sizes=ICO_SIZES)
    print(f"wrote packaging/explosionsim.ico ({len(ICO_SIZES)} sizes)")

    write_png(source, ROOT / "packaging" / "explosionsim.png", 256)
    write_png(source, ROOT / "packaging" / "explosionsim-512.png", 512)
    write_png(source, ROOT / "assets" / "icon.png", 256)


if __name__ == "__main__":
    main()
