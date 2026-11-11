#!/bin/bash
# Build report.html from the raw runs, render the PDF with headless Chrome, rasterize pages for QA.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$R/.chrome-tmp" "$R/qa"
export TMPDIR="$R/.chrome-tmp"
python3 "$R/build.py" > "$R/qa/build_log.txt"
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless --disable-gpu \
  --no-pdf-header-footer --no-first-run --disable-background-networking \
  --user-data-dir="$R/.chrome-tmp/profile" \
  --print-to-pdf="$R/rowdb-m1-report.pdf" "file://$R/report.html" > "$R/qa/chrome_log.txt" 2>&1
python3 "$R/qa_check.py"
