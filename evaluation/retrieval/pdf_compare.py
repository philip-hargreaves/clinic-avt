"""PDFium text extraction against pdftotext on the client PDFs.

  python pdf_compare.py [--dir rag/sources/st-georges/folder] [--pdftotext PATH]

Writes rag/results/<stamp>-pdf-compare/pdf-compare.csv
"""

import argparse
import csv
import difflib
import os
import re
import shutil
import sys
from pathlib import Path

import pypdfium2 as pdfium

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import log  # noqa: E402
from harness import CLIENT_PDFS, pdftotext, run_dir  # noqa: E402

REC = re.compile(r"(^|\n)\s*(Recommendation\s+\d+[a-z]?\b|\d+\.\d+(\.\d+)?\s+[A-Z])")


def words(text: str) -> list[str]:
    return re.findall(r"[A-Za-z][A-Za-z\-']+", text)


def pdfium_text(path: Path) -> tuple[str, int, int]:
    doc = pdfium.PdfDocument(str(path))
    parts, chars, images = [], 0, 0
    for page in doc:
        tp = page.get_textpage()
        parts.append(tp.get_text_bounded())
        chars += tp.count_chars()
        images += sum(1 for o in page.get_objects() if o.type == pdfium.raw.FPDF_PAGEOBJ_IMAGE)
    return "\n".join(parts), chars, images


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=str(CLIENT_PDFS))
    ap.add_argument("--pdftotext", default=shutil.which("pdftotext"))
    args = ap.parse_args()
    if not args.pdftotext:
        raise SystemExit("pdftotext not on PATH; pass --pdftotext")

    out = run_dir("pdf-compare", vars(args))
    rows = []
    for pdf in sorted(Path(args.dir).glob("*.pdf")):
        a, chars, images = pdfium_text(pdf)
        b = pdftotext(pdf, args.pdftotext)
        wa, wb = words(a), words(b)
        jaccard = len(set(w.lower() for w in wa) & set(w.lower() for w in wb)) / max(1, len(set(w.lower() for w in wa) | set(w.lower() for w in wb)))
        order = difflib.SequenceMatcher(None, [w.lower() for w in wa[:3000]], [w.lower() for w in wb[:3000]]).ratio()
        rows.append({"file": pdf.name, "pages": len(pdfium.PdfDocument(str(pdf))), "pdfium_chars": chars,
                     "pdfium_words": len(wa), "pdftotext_words": len(wb), "word_jaccard": round(jaccard, 3),
                     "order_ratio_3000": round(order, 3), "rec_patterns_pdfium": len(REC.findall(a)),
                     "rec_patterns_pdftotext": len(REC.findall(b)), "image_only": int(chars == 0 and images > 0)})
        log(str(rows[-1]))
    with open(out / "pdf-compare.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    log(f"done -> {out}")


if __name__ == "__main__":
    main()
