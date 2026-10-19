#!/usr/bin/env python3
"""The homebrew cover (coverflow of the Retro-Go launcher) from a screenshot.

The firmware caps covers at 186x100 and 10 KiB (HW JPEG scratch): the
screenshot (320x240) is scaled to fit, keeping its 4:3 shape, then saved as
a baseline JPEG at the best quality that fits in 10 KiB.
"""

from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path

from PIL import Image

COVER_MAX_WIDTH = 186
COVER_MAX_HEIGHT = 100
COVER_SIZE_MAX = 10 * 1024


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="src", type=Path, required=True, help="screenshot (PNG)")
    ap.add_argument("--out", type=Path, required=True, help="output JPEG")
    ap.add_argument("--height", type=int, default=COVER_MAX_HEIGHT)
    args = ap.parse_args()

    im = Image.open(args.src).convert("RGB")
    h = min(args.height, COVER_MAX_HEIGHT)
    w = min(COVER_MAX_WIDTH, round(im.width * h / im.height))
    im = im.resize((w, h), Image.LANCZOS)
    for q in range(95, 30, -5):
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=q, optimize=True, progressive=False)
        if buf.tell() <= COVER_SIZE_MAX:
            args.out.write_bytes(buf.getvalue())
            print(f"cover: {w}x{h}, quality {q}, {buf.tell()} bytes")
            return 0
    print("cover: cannot fit in 10 KiB", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
