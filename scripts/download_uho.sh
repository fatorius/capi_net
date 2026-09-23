#!/usr/bin/env bash
#
# Downloads the UHO (Unbalanced Human Openings) test book, a popular public
# opening suite used by engine testers (OpenBench, CCRL, etc.) for SPRT work.
#
# More diverse than starter.epd and designed to avoid boring draws. Prefer
# this book over starter.epd once you're running serious SPRTs.
#
# Usage (from repo root):
#   bash scripts/download_uho.sh
#
# The script downloads to books/ (the server's book directory). If the URL below breaks, check
# https://github.com/official-stockfish/books for an up-to-date mirror and
# update BOOK_URL.

set -euo pipefail

BOOK_URL="https://github.com/official-stockfish/books/raw/master/UHO_Lichess_4852_v1.epd.zip"
BOOK_DIR="$(cd "$(dirname "$0")/.." && pwd)/books"
mkdir -p "$BOOK_DIR"
ARCHIVE="$BOOK_DIR/UHO_Lichess_4852_v1.epd.zip"
EXTRACTED="$BOOK_DIR/UHO_Lichess_4852_v1.epd"

if [ -f "$EXTRACTED" ]; then
    echo "already present: $EXTRACTED"
    exit 0
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl not found" >&2
    exit 1
fi

if ! command -v unzip >/dev/null 2>&1; then
    echo "error: unzip not found" >&2
    exit 1
fi

echo "fetching $BOOK_URL ..."
curl -fL --retry 3 -o "$ARCHIVE" "$BOOK_URL"

echo "extracting ..."
unzip -o "$ARCHIVE" -d "$BOOK_DIR"
rm -f "$ARCHIVE"

if [ ! -f "$EXTRACTED" ]; then
    echo "error: extraction did not produce $EXTRACTED" >&2
    echo "the archive layout may have changed; extract manually from $BOOK_DIR" >&2
    exit 1
fi

lines=$(wc -l < "$EXTRACTED")
echo "done: $EXTRACTED ($lines positions)"
