"""Convert a Tiled TMX map to tile_editor JSON format.

Usage:
    python tmx_to_editor.py Tilemap/sample_fantasy.tmx -o maps/sample_fantasy.json
    python tmx_to_editor.py Tilemap/*.tmx              # outputs next to each .tmx
"""

import argparse
import xml.etree.ElementTree as ET
from pathlib import Path
import json

# Tiled flip flags (upper bits of raw GID)
FLIPPED_HORIZONTALLY = 0x80000000
FLIPPED_VERTICALLY = 0x40000000
FLIPPED_DIAGONALLY = 0x20000000
GID_MASK = 0x1FFFFFFF


def tiled_flags_to_transform(raw_gid: int) -> tuple[int, int, bool, bool]:
    """Convert Tiled raw GID to (base_gid, rotation, flip_h, flip_v).

    Tiled uses 3 flag bits: H (horizontal flip), V (vertical flip), D (diagonal/transpose).
    These combine to represent rotations and flips:

        Flags  → Meaning                → Editor (rotation, flip_h, flip_v)
        (none) → identity               → (0,   False, False)
        H      → horizontal flip        → (0,   True,  False)
        V      → vertical flip          → (0,   False, True)
        HV     → 180° rotation          → (180, False, False)
        D      → diagonal transpose     → (90,  False, True)   *
        HD     → 90° CW rotation        → (90,  False, False)
        VD     → 270° CW rotation       → (270, False, False)
        HVD    → anti-diagonal flip      → (270, True,  False)  *

    * D alone and HVD are transpose/anti-transpose, which are rotation+flip combos.
    """
    base = raw_gid & GID_MASK
    h = bool(raw_gid & FLIPPED_HORIZONTALLY)
    v = bool(raw_gid & FLIPPED_VERTICALLY)
    d = bool(raw_gid & FLIPPED_DIAGONALLY)

    # Map (H, V, D) combinations to (rotation, flip_h, flip_v)
    # These are derived by matching the visual result of Tiled's transforms
    # to the editor's rotate-then-flip model.
    lookup = {
        (False, False, False): (0,   False, False),
        (True,  False, False): (0,   True,  False),
        (False, True,  False): (0,   False, True),
        (True,  True,  False): (180, False, False),
        (False, False, True):  (90,  False, True),
        (True,  False, True):  (90,  False, False),
        (False, True,  True):  (270, False, False),
        (True,  True,  True):  (270, True,  False),
    }

    rotation, flip_h, flip_v = lookup[(h, v, d)]
    return base, rotation, flip_h, flip_v


def convert_tmx(tmx_path: Path) -> dict:
    """Parse TMX and return editor-format dict."""
    tree = ET.parse(tmx_path)
    root = tree.getroot()
    width = int(root.attrib["width"])
    height = int(root.attrib["height"])

    layer = root.find(".//layer/data")
    if layer is None or layer.attrib.get("encoding") != "csv":
        raise ValueError(f"{tmx_path.name}: expected CSV-encoded layer data")

    raw_values = [int(v.strip()) for v in layer.text.strip().split(",") if v.strip()]

    grid = []
    transforms = []
    for row in range(height):
        grid_row = []
        transform_row = []
        for col in range(width):
            raw = raw_values[row * width + col]
            base, rotation, flip_h, flip_v = tiled_flags_to_transform(raw)
            grid_row.append(base)
            transform_row.append([rotation, flip_h, flip_v])
        grid.append(grid_row)
        transforms.append(transform_row)

    return {
        "width": width,
        "height": height,
        "grid": grid,
        "transforms": transforms,
    }


def main():
    parser = argparse.ArgumentParser(description="Convert TMX maps to tile_editor JSON format")
    parser.add_argument("tmx_files", nargs="+", type=Path, help="TMX file(s) to convert")
    parser.add_argument("-o", "--output", type=Path, default=None,
                        help="Output JSON path (only for single file; ignored for multiple)")
    args = parser.parse_args()

    for tmx_path in args.tmx_files:
        if not tmx_path.exists():
            print(f"  SKIP: {tmx_path} not found")
            continue

        data = convert_tmx(tmx_path)

        if args.output and len(args.tmx_files) == 1:
            out_path = args.output
        else:
            out_path = tmx_path.with_suffix(".json")

        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(data, indent=2))

        # Count transformed tiles
        n_transformed = sum(
            1 for row in data["transforms"]
            for rot, fh, fv in row
            if rot != 0 or fh or fv
        )
        print(f"  {tmx_path.name} → {out_path}")
        print(f"    {data['width']}x{data['height']}, {n_transformed} transformed tiles")


if __name__ == "__main__":
    main()
