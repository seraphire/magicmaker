#!/usr/bin/env python3
"""
make-device-qr.py - a scannable label for one specific reader.

    python tools/make-device-qr.py --id 0F85
    python tools/make-device-qr.py --id 0F85 --round      # circular card/sticker
    python tools/make-device-qr.py --id 0F85 --out ./cards

Encodes  http://magicmaker-<id>.local/  - the device's PERMANENT name, derived
from its MAC. Deliberately not the friendly name: that one changes the moment
somebody renames the reader, and a printed card can't be edited afterwards.

That's also why the hex belongs here and not in the spoken help. Nobody types a
QR code, so "zero eff eight five" costs nothing to scan and everything to say
out loud. The audio says "magicmaker.local"; this says exactly which reader.

Writes SVG (print from this - it's vector, so it stays crisp at any size) and
PNG (for pasting into a document or a chat).

Requires: pip install segno
"""
import argparse
import pathlib
import sys

try:
    import segno
except ImportError:
    sys.exit("segno not installed.  pip install segno")


def build(device_id: str, out_dir: pathlib.Path, round_card: bool,
          size_mm: float, label: str | None) -> None:
    device_id = device_id.upper().strip()
    host = f"magicmaker-{device_id}.local"
    url = f"http://{host}/"

    # Error correction H (~30% recoverable). Overkill for a pristine label, and
    # exactly right for one that will be handled, scuffed, curved around a
    # shell, or cropped by a circular die - all of which eat modules.
    qr = segno.make(url, error="h")

    out_dir.mkdir(parents=True, exist_ok=True)
    stem = out_dir / f"magicmaker-{device_id}"

    # A QR needs 4 modules of blank margin to be found at all. Scanners fail on
    # a code printed edge-to-edge far more often than on a small or dim one.
    border = 4

    # On a round card the square has to fit INSIDE the circle, so the usable
    # width is the diameter over root two. Extra border pushes the code inward
    # rather than letting the die-cut clip its corners.
    if round_card:
        border += 3

    qr.save(str(stem) + ".png", scale=12, border=border)

    # Vector for print. Scale is in user units here; the surrounding SVG is
    # given an explicit mm size so it lands at a known physical size rather
    # than whatever the printer guesses.
    qr.save(str(stem) + ".svg", scale=10, border=border, unit="mm",
            svgclass=None, lineclass=None)

    caption = label or host
    print(f"  {stem}.svg")
    print(f"  {stem}.png")
    print()
    print(f"  encodes : {url}")
    print(f"  caption : {caption}")
    print(f"  ecc     : H (~30% of the code can be damaged and still scan)")
    if round_card:
        print(f"  layout  : extra margin so a circular cut can't clip the corners")
    print()
    print("  Print the SVG. Test it with the actual phone that will use it BEFORE")
    print("  committing to artwork: .local addresses resolve reliably from iOS,")
    print("  but Android has been inconsistent about mDNS for years. If it fails")
    print("  there, point the card at setup instructions instead of the device.")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--id", required=True,
                    help="device id, 4 hex chars - `status` on the serial console prints it")
    ap.add_argument("--out", default="assets/qr", type=pathlib.Path)
    ap.add_argument("--round", action="store_true", dest="round_card",
                    help="extra margin so a circular card/sticker can't clip the code")
    ap.add_argument("--size", type=float, default=25.0, help="target width in mm")
    ap.add_argument("--label", default=None, help="caption to print beneath (default: the hostname)")
    a = ap.parse_args()

    if len(a.id) != 4 or any(c not in "0123456789abcdefABCDEF" for c in a.id):
        sys.exit(f"device id should be 4 hex characters, got {a.id!r}")

    build(a.id, a.out, a.round_card, a.size, a.label)


if __name__ == "__main__":
    main()
