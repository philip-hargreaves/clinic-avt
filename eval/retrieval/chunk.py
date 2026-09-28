"""Build chunk files from the NICE JSON corpus and from PDF text.

  python chunk.py nice                 -> rag/results/chunks/nice-<fetchdate>.jsonl
  python chunk.py pdf <dir> [--pdftotext PATH]  -> rag/results/chunks/pdf-<dirname>.jsonl
"""

import argparse
import os
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import log, read_json, write_jsonl  # noqa: E402
from harness import NICE_JSON, NICE_MANIFEST, RESULTS, pdftotext  # noqa: E402

REC_PATTERN = re.compile(r"^(Recommendation\s+\d+[a-z]?|\d+\.\d+(\.\d+)*)\b", re.I)
TARGET_WORDS = 300
MAX_WORDS = 600


def nice_chunks():
    manifest = read_json(NICE_MANIFEST)
    requested = {line.split("#")[0].strip().lower() for line in (NICE_JSON.parents[1] / "codes.txt").read_text(encoding="utf-8").splitlines()}
    requested.discard("")
    fetch_dates, skipped, duplicates, seen = set(), 0, 0, set()
    for path in sorted(NICE_JSON.glob("*.json")):
        doc = read_json(path)
        code = doc["code"]
        entry = manifest.get(code, {})
        if code not in requested or entry.get("is_stub") or entry.get("status") != "current":
            skipped += 1
            continue
        fetch_dates.add(entry.get("fetched_at", "")[:10])
        for chapter in doc["chapters"]:
            for rec in chapter["recommendations"]:
                if rec.get("kind") != "recommendation":
                    continue
                # Some guidelines render a chapter twice (NG12, NG126, NG259). One chunk per id
                if rec["id"] in seen:
                    duplicates += 1
                    continue
                seen.add(rec["id"])
                section = rec.get("section", "")
                text = rec["text"].strip()
                yield {
                    "id": rec["id"],
                    "code": code,
                    "title": doc["title"],
                    "chapter": chapter["title"],
                    "number": rec["number"],
                    "section": section,
                    "update_tag": rec.get("update_tag", ""),
                    "last_updated": doc.get("last_updated", ""),
                    "text": text,
                    "text_prefixed": f"{doc['title']}. {section}. {text}" if section else f"{doc['title']}. {text}",
                    "url": f"{doc['source_url']}/chapter/{chapter['slug']}#{rec['id']}",
                    "source": "nice",
                }
    log(f"fetch dates seen: {sorted(fetch_dates)}; stub or superseded files skipped: {skipped}; duplicate renderings skipped: {duplicates}")


def pdf_chunk(pdf: Path, n: int, buffer: list[str]) -> dict:
    text = " ".join(buffer)
    return {"id": f"{pdf.stem}-{n}", "code": pdf.stem, "title": pdf.stem, "number": "",
            "section": "", "text": text, "text_prefixed": f"{pdf.stem}. " + text,
            "url": pdf.name, "source": "pdf"}


def pdf_chunks(pdf_dir: Path, exe: str):
    for pdf in sorted(pdf_dir.glob("*.pdf")):
        text = pdftotext(pdf, exe)
        paragraphs = [re.sub(r"\s+", " ", p).strip() for p in re.split(r"\n\s*\n", text)]
        paragraphs = [p for p in paragraphs if len(p.split()) >= 5]
        if not paragraphs:
            log(f"{pdf.name}: no text layer")
            continue
        buffer, n = [], 0
        for para in paragraphs:
            starts_rec = bool(REC_PATTERN.match(para))
            words = len(para.split())
            if buffer and (starts_rec or sum(len(b.split()) for b in buffer) + words > MAX_WORDS
                           or sum(len(b.split()) for b in buffer) >= TARGET_WORDS):
                n += 1
                yield pdf_chunk(pdf, n, buffer)
                buffer = []
            buffer.append(para)
        if buffer:
            n += 1
            yield pdf_chunk(pdf, n, buffer)
        log(f"{pdf.name}: {n} chunks")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("nice")
    p = sub.add_parser("pdf")
    p.add_argument("dir")
    p.add_argument("--pdftotext", default=shutil.which("pdftotext"))
    args = ap.parse_args()

    if args.cmd == "nice":
        manifest = read_json(NICE_MANIFEST)
        fetch_date = min(v.get("fetched_at", "")[:10] for v in manifest.values() if v.get("fetched_at"))
        out = RESULTS / "chunks" / f"nice-{fetch_date}.jsonl"
        n = write_jsonl(out, nice_chunks())
        log(f"{n} chunks -> {out}")
    else:
        pdf_dir = Path(args.dir)
        out = RESULTS / "chunks" / f"pdf-{pdf_dir.name}.jsonl"
        n = write_jsonl(out, pdf_chunks(pdf_dir, args.pdftotext))
        log(f"{n} chunks -> {out}")


if __name__ == "__main__":
    main()
