#!/usr/bin/env python3
"""Rasterize every page of the PDF and print per-page structure (size, text amount, top/bottom of content)."""
import json
import pathlib

import fitz

R = pathlib.Path(__file__).resolve().parent
doc = fitz.open(R / "rowdb-m1-report.pdf")
qa = R / "qa"
for old in qa.glob("page-*.png"):
    old.unlink()
out = {"pages": len(doc), "per_page": []}
for i, page in enumerate(doc):
    page.get_pixmap(dpi=110).save(qa / f"page-{i + 1}.png")
    blocks = [b for b in page.get_text("blocks") if b[4].strip()]
    text = page.get_text().strip()
    out["per_page"].append({
        "page": i + 1,
        "size_mm": [round(page.rect.width / 72 * 25.4), round(page.rect.height / 72 * 25.4)],
        "chars": len(text),
        "first": text.splitlines()[0][:40] if text else "",
        "last": text.splitlines()[-1][:40] if text else "",
        "content_bottom_mm": round(max(b[3] for b in blocks) / 72 * 25.4) if blocks else None,
    })
fonts = sorted({f[3] for p in doc for f in p.get_fonts()})
out["fonts"] = fonts
print(json.dumps(out, ensure_ascii=False, indent=1))
