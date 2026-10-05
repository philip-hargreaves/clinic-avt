"""Anonymisation sweep for the appraisal case summary: prompts/case-summary.md over each tier's
concise notes (standard in banked runs), each summary then scanned with the app's identifier
patterns plus weekdays, months, age-like numbers and clinician mentions. Capitalised mid-sentence
words that are not common clinical terms are listed to read.

  notes/case-summary-<tier>/<cid>.md, notes/case-summary-timings.jsonl
  build/evaluation/summarisation/case-summary-scan.md

    python evaluation/summarisation/case_summary.py [--tier default] [--limit N]
    python evaluation/summarisation/case_summary.py --scan
"""
import argparse
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import append_jsonl  # noqa: E402

NOTES = str(config.path("summarisation") / "notes")
TIMINGS = os.path.join(NOTES, f"{os.environ.get('CASE_OUT', 'case-summary')}-timings.jsonl")
TIERS = ["constrained", "default", "accuracy"]
# CASE_SOURCE is the notes folder to summarise, with {tier} filled in. CASE_OUT prefixes the output
SOURCE = os.environ.get("CASE_SOURCE", "tier-{tier}-concise")
PREFIX = os.environ.get("CASE_OUT", "case-summary")

# The app's IdentifierCheck, mirrored, plus what a summary must not do
PATTERNS = {
    "titled name": re.compile(r"\b(?:Mr|Mrs|Ms|Miss|Mx|Dr|Prof)\.?\s+[A-Z][a-z]+"),
    "numeric date": re.compile(r"\b\d{1,2}[/.\-]\d{1,2}[/.\-]\d{2,4}\b"),
    "written date": re.compile(
        r"\b\d{1,2}(?:st|nd|rd|th)?\s+(?:January|February|March|April|May|June|July|August|"
        r"September|October|November|December)\b", re.I),
    "exact age": re.compile(r"\b(?:aged\s+\d{1,3}|\d{1,3}[\s-]year[\s-]old|\d{1,3}\s*y/?o)\b", re.I),
    "nhs number": re.compile(r"\b\d{3}[ \-]?\d{3}[ \-]?\d{4}\b"),
    "postcode": re.compile(r"\b[A-Z]{1,2}\d[A-Z\d]?\s*\d[A-Z]{2}\b"),
    "phone": re.compile(r"\b0\d{2,4}[ \-]?\d{3,4}[ \-]?\d{3,4}\b"),
    "weekday or month": re.compile(
        r"\b(?:Monday|Tuesday|Wednesday|Thursday|Friday|Saturday|Sunday|January|February|March|"
        r"April|June|July|August|September|October|November|December)\b"),
    "bare number that could be an age": re.compile(r"\b(?:age|aged|at)\s+\d{1,3}\b", re.I),
    "clinician mentioned": re.compile(r"\b(?:doctor|clinician|GP|nurse)\b", re.I),
}
# Capitalised mid-sentence words outside this set are listed as candidate names to read
COMMON = set("""The A An On In At No Patient Olecranon Paracetamol Ibuprofen Naproxen NSAID NSAIDs GP
UTI COPD BP ECG MRI CT NHS Type Covid COVID Amoxicillin Trimethoprim Nitrofurantoin Omeprazole
Sertraline Citalopram Fluoxetine Salbutamol Metformin Ramipril Amlodipine Atorvastatin Codeine
Tramadol Diazepam Lansoprazole Prednisolone Flucloxacillin Doxycycline Clarithromycin Cetirizine
Loratadine Chlorphenamine Gaviscon Movicol Lactulose Senna Fybogel Voltarol Deep Heat Night
Nurse Lemsip Nurofen Calpol Piriton Sudafed Vicks Otex Canesten Daktarin Anusol E45 Diprobase
Cetraben Dermol Hydrocortisone Eumovate Betnovate Dermovate Fucidin Ventolin Clenil Symbicort
Fostair Seretide Spiriva Relvar Trelegy Nasonex Beconase Flixonase Avamys Dymista Optrex Chloramphenicol
Fusidic Otomize Sofradex Otosporin Earol Canestan Nizoral Selsun Pepto Rennie Imodium Buscopan
Colofac Mebeverine Peppermint Nausea Emergency Department""".split())


def out_dir(tier):
    d = os.path.join(NOTES, f"{PREFIX}-{tier}")
    os.makedirs(d, exist_ok=True)
    return d


def notes_for(tier):
    out = []
    paths = sorted(glob.glob(os.path.join(NOTES, SOURCE.format(tier=tier), "*.md")))
    for path in paths or sorted(glob.glob(os.path.join(NOTES, f"tier-{tier}-standard", "*.md"))):
        text = open(path, encoding="utf-8").read().strip()
        if not text or text.startswith("NOT A CONSULTATION"):
            continue
        out.append((os.path.basename(path)[:-3], text))
    return out


def run(tier, limit):
    from common.llm import NoteModel
    prompt = (config.path("prompts") / "case-summary.md").read_text(encoding="utf-8")
    model_id = config.section("summarisation")["tiers"][tier]
    model = NoteModel(model_id)
    print(f"{tier}: {model_id} loaded in {model.load_s:.1f} s", flush=True)
    done = 0
    for cid, note in notes_for(tier):
        target = os.path.join(out_dir(tier), f"{cid}.md")
        if os.path.exists(target):
            continue
        text, m = model.generate(prompt + note + "\n", 160)
        open(target, "w", encoding="utf-8").write(text + "\n")
        m.update({"tier": tier, "model": model_id, "cid": cid, "words": len(text.split())})
        append_jsonl(TIMINGS, m)
        done += 1
        print(f"  {cid}: {m['words']} words, {m['wall_s']} s", flush=True)
        if limit and done >= limit:
            break


def scan():
    lines = ["# Case-summary anonymisation sweep", "",
             "`prompts/case-summary.md` over the concise clinical notes of each tier (the app's own",
             "notes from the sealed 1x transcripts). Scanned with the app's identifier patterns plus",
             "weekday/month, age-like numbers and clinician mentions. Capitalised mid-sentence words",
             "that are not common clinical terms are listed for reading.", ""]
    timings = {}
    if os.path.exists(TIMINGS):
        for l in open(TIMINGS, encoding="utf-8"):
            r = json.loads(l)
            timings.setdefault(r["tier"], []).append(r)
    summary_rows = []
    details = []
    for tier in TIERS:
        d = os.path.join(NOTES, f"{PREFIX}-{tier}")
        if not os.path.isdir(d):
            continue
        files = sorted(glob.glob(os.path.join(d, "*.md")))
        hits = {k: 0 for k in PATTERNS}
        flagged = 0
        words = []
        decade = 0
        candidates = []
        for path in files:
            text = open(path, encoding="utf-8").read().strip()
            cid = os.path.basename(path)[:-3]
            words.append(len(text.split()))
            if re.search(r"\bin (?:their|his|her) (?:teens|twenties|thirties|forties|fifties|sixties|seventies|eighties|nineties)\b", text):
                decade += 1
            found = []
            for k, p in PATTERNS.items():
                for m in p.finditer(text):
                    hits[k] += 1
                    found.append(f"{k}: {m.group(0)}")
            if found:
                flagged += 1
                details.append(f"- **{tier} {cid}**: " + "; ".join(found))
            for m in re.finditer(r"(?<=[a-z,;] )([A-Z][a-z]{2,})", text):
                w = m.group(1)
                if w not in COMMON:
                    candidates.append(f"{cid}: {w}")
        t = timings.get(tier, [])
        wall = sorted(r["wall_s"] for r in t) if t else []
        summary_rows.append((tier, len(files), flagged, decade,
                             f"{sorted(words)[len(words)//2] if words else 0} ({min(words) if words else 0}-{max(words) if words else 0})",
                             sum(1 for w in words if w > 60),
                             f"{wall[len(wall)//2]:.1f}" if wall else "-",
                             hits, candidates))
    lines += ["| | " + " | ".join(r[0] for r in summary_rows) + " |",
              "|---|" + "---|" * len(summary_rows),
              "| summaries | " + " | ".join(str(r[1]) for r in summary_rows) + " |",
              "| flagged by any pattern | " + " | ".join(str(r[2]) for r in summary_rows) + " |",
              "| age given as a decade | " + " | ".join(str(r[3]) for r in summary_rows) + " |",
              "| words, median (min-max) | " + " | ".join(r[4] for r in summary_rows) + " |",
              "| over 60 words | " + " | ".join(str(r[5]) for r in summary_rows) + " |",
              "| wall per summary, median s | " + " | ".join(r[6] for r in summary_rows) + " |"]
    for k in PATTERNS:
        lines.append(f"| {k} | " + " | ".join(str(r[7][k]) for r in summary_rows) + " |")
    lines += ["", "## Flagged summaries", ""] + (details or ["None."])
    lines += ["", "## Capitalised mid-sentence words to read (not counted)", ""]
    for r in summary_rows:
        lines.append(f"- {r[0]}: " + (", ".join(r[8][:40]) if r[8] else "none"))
    out = os.path.join(config.out("summarisation"), f"{PREFIX}-scan.md")
    open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=TIERS)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--scan", action="store_true")
    args = ap.parse_args()
    if args.scan:
        scan()
        return
    for tier in ([args.tier] if args.tier else TIERS):
        run(tier, args.limit)
    scan()


if __name__ == "__main__":
    main()
