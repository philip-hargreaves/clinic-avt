"""Deterministic metrics over app-mode notes (generate.py app), one column per tag.

Per tag and detail level: word and sentence counts against the prompt's stated limits,
paragraph counts, refusals, forbidden words; per patient sheet: the four headings in
order, sentence length rule (about 10, never more than 15), length against the ~200
target, readability (Flesch-Kincaid grade, SMOG), reassurance phrases; per title: two to
five words, no sentence punctuation. Timings from notes/timings.jsonl. Prints markdown.

    python evaluation/summarisation/metrics.py > build/evaluation/summarisation/tier-metrics.md
    python evaluation/summarisation/metrics.py --tags tier-default,gemma-4-31b-it-int4-ov
"""
import argparse
import os
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import read_jsonl  # noqa: E402

TIERS = ["tier-constrained", "tier-default", "tier-accuracy"]
LIMITS = {"concise": (100, 6), "detailed": (200, 12)}
# The three lengths of the banked tier baseline
BASELINE_LIMITS = {"concise": (60, 5), "standard": (110, 8), "detailed": (220, 15)}
HEADINGS = ["Your appointment today", "Your diagnosis", "Your treatment and next steps", "When to contact us"]
REASSURANCE = re.compile(r"\b(not serious|nothing to worry|no need to worry|not dangerous|not an emergency|is harmless|reassur)", re.I)


def texts(kind):
    folder = config.path("summarisation") / "notes" / kind
    return {p.stem: p.read_text(encoding="utf-8").strip() for p in sorted(folder.glob("*.md"))}


def words(t):
    return len(t.split())


def sentences(t):
    parts = [s for s in re.split(r"(?<=[.!?])\s+", t.replace("\n", " ").strip()) if s.strip()]
    return parts


def paragraphs(t):
    return [p for p in re.split(r"\n\s*\n", t.strip()) if p.strip()]


def syllables(word):
    w = re.sub(r"[^a-z]", "", word.lower())
    if not w:
        return 0
    groups = re.findall(r"[aeiouy]+", w)
    n = len(groups)
    if w.endswith("e") and not w.endswith(("le", "ee")) and n > 1:
        n -= 1
    return max(1, n)


def readability(t):
    sents = sentences(t)
    ws = [w for w in re.findall(r"[A-Za-z][A-Za-z'-]*", t)]
    if not sents or not ws:
        return None, None
    syl = [syllables(w) for w in ws]
    fk = 0.39 * len(ws) / len(sents) + 11.8 * sum(syl) / len(ws) - 15.59
    poly = sum(1 for s in syl if s >= 3)
    smog = 1.043 * (poly * 30 / len(sents)) ** 0.5 + 3.1291
    return round(fk, 1), round(smog, 1)


def pct(n, d):
    return f"{100 * n / d:.0f} %" if d else "-"


def med(xs):
    return statistics.median(xs) if xs else None


def fmt(x, unit=""):
    return "-" if x is None else (f"{x:.1f}{unit}" if isinstance(x, float) else f"{x}{unit}")


def timings(path):
    # Records carry their tag; the first baseline run recorded the tier instead
    rows = read_jsonl(path) if path.exists() else []
    for r in rows:
        r.setdefault("tag", f"tier-{r.get('tier')}")
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tags", default=",".join(TIERS), help="comma list of note tags, one column each")
    ap.add_argument("--timings", help="generation records (default notes/timings.jsonl)")
    ap.add_argument("--baseline", action="store_true", help="the banked baseline's three lengths")
    args = ap.parse_args()
    tiers = config.section("summarisation")["tiers"]
    columns = [(t, config.label(tiers.get(t.removeprefix("tier-"), t))) for t in args.tags.split(",")]
    tim = timings(Path(args.timings) if args.timings else config.path("summarisation") / "notes" / "timings.jsonl")
    tag = config.section("summarisation")["transcript_tag"]
    print("# App-mode notes: deterministic metrics\n")
    print(f"Sealed transcripts of the 57 PriMock consultations (perf-loop tag {tag}), "
          "the app's own prompts and composition, greedy decoding.\n")

    for detail, (wlimit, slimit) in (BASELINE_LIMITS if args.baseline else LIMITS).items():
        print(f"## Clinical note, {detail} (limit {wlimit} words, {slimit} sentences)\n")
        print("| | " + " | ".join(n for _, n in columns) + " |")
        print("|---|" + "---|" * len(columns))
        rows = {}
        for tier, _ in columns:
            ts = texts(f"{tier}-{detail}")
            notes = {c: t for c, t in ts.items() if not t.startswith("NOT A CONSULTATION")}
            refused = len(ts) - len(notes)
            empty = sum(1 for t in notes.values() if not t.strip())
            notes = {c: t for c, t in notes.items() if t.strip()}
            wc = [words(t) for t in notes.values()]
            sc = [len(sentences(t)) for t in notes.values()]
            pc = [len(paragraphs(t)) for t in notes.values()]
            forbidden = sum(1 for t in notes.values() if re.search(r"\b(doctor|clinician)\b", t, re.I))
            gen = [r for r in tim if r.get("tag") == tier and r.get("kind") == detail]
            rows[tier] = {
                "consultations": len(ts),
                "refused": refused,
                "empty output": empty,
                "within word limit": pct(sum(1 for w in wc if w <= wlimit), len(wc)),
                "within sentence limit": pct(sum(1 for s in sc if s <= slimit), len(sc)),
                "words, median (min-max)": f"{fmt(med(wc))} ({min(wc) if wc else '-'}-{max(wc) if wc else '-'})",
                "worst overshoot": f"+{max(wc) - wlimit} words" if wc and max(wc) > wlimit else "none",
                "sentences, median": fmt(med(sc)),
                "paragraphs, median": fmt(med(pc)),
                "mentions doctor/clinician": pct(forbidden, len(notes)),
                "generation wall, median": fmt(med([g["wall_s"] for g in gen]), " s"),
                "first token, median": fmt(med([g["ttft_s"] for g in gen]), " s"),
                "decode, median": fmt(med([1000 / g["tpot_ms"] for g in gen if g.get("tpot_ms")]), " tok/s"),
            }
        for key in next(iter(rows.values())):
            print(f"| {key} | " + " | ".join(str(rows[t][key]) for t, _ in columns) + " |")
        print()

    print("## Patient sheet (from the standard note; about 200 words, sentences about 10 words, never more than 15)\n")
    print("| | " + " | ".join(n for _, n in columns) + " |")
    print("|---|" + "---|" * len(columns))
    rows = {}
    for tier, _ in columns:
        ts = texts(f"{tier}-sheet")
        wc = [words(t) for t in ts.values()]
        sl = [words(s) for t in ts.values() for s in sentences(t)]
        over15 = sum(1 for n in sl if n > 15)
        heads_ok = 0
        order_ok = 0
        assessment_with_dx = 0
        diagnosis_heading = 0
        for t in ts.values():
            # The second heading is "Your diagnosis", or "Your assessment" only when
            # the note names no diagnosis; either counts as present here
            second = t.find("Your diagnosis")
            if second >= 0:
                diagnosis_heading += 1
            else:
                second = t.find("Your assessment")
                if second >= 0 and re.search(r"diagnos", t[second:second + 400], re.I):
                    assessment_with_dx += 1
            pos = [t.find(HEADINGS[0]), second, t.find(HEADINGS[2]), t.find(HEADINGS[3])]
            if all(p >= 0 for p in pos):
                heads_ok += 1
                if pos == sorted(pos):
                    order_ok += 1
        fk = [readability(t)[0] for t in ts.values()]
        smog = [readability(t)[1] for t in ts.values()]
        gloss = [len(re.findall(r"\([a-z][a-z -]+\)", t)) for t in ts.values()]
        reass = sum(1 for t in ts.values() if REASSURANCE.search(t))
        gen = [r for r in tim if r.get("tag") == tier and r.get("kind") == "sheet"]
        rows[tier] = {
            "sheets": len(ts),
            "all four headings present (diagnosis or assessment)": pct(heads_ok, len(ts)),
            "headings in order": pct(order_ok, len(ts)),
            "used the 'Your diagnosis' heading": pct(diagnosis_heading, len(ts)),
            "wrote 'Your assessment' yet names a diagnosis": pct(assessment_with_dx, len(ts)),
            "words, median (min-max)": f"{fmt(med(wc))} ({min(wc) if wc else '-'}-{max(wc) if wc else '-'})",
            "within 150-250 words": pct(sum(1 for w in wc if 150 <= w <= 250), len(wc)),
            "sentence length, median": fmt(med(sl)),
            "sentences over 15 words": pct(over15, len(sl)),
            "Flesch-Kincaid grade, median": fmt(med([x for x in fk if x is not None])),
            "SMOG grade, median": fmt(med([x for x in smog if x is not None])),
            "bracketed glosses, median": fmt(med(gloss)),
            "reassurance phrases present": pct(reass, len(ts)),
            "generation wall, median": fmt(med([g["wall_s"] for g in gen]), " s"),
        }
    for key in next(iter(rows.values())):
        print(f"| {key} | " + " | ".join(str(rows[t][key]) for t, _ in columns) + " |")
    print()

    print("## Title (two to five words, no sentence punctuation)\n")
    print("| | " + " | ".join(n for _, n in columns) + " |")
    print("|---|" + "---|" * len(columns))
    rows = {}
    for tier, _ in columns:
        ts = texts(f"{tier}-label")
        wc = [words(t) for t in ts.values()]
        rows[tier] = {
            "titles": len(ts),
            "two to five words": pct(sum(1 for w in wc if 2 <= w <= 5), len(wc)),
            "empty": pct(sum(1 for t in ts.values() if not t), len(ts)),
            "contains . : or quotes": pct(sum(1 for t in ts.values() if re.search(r"[.:\"']", t)), len(ts)),
        }
    for key in next(iter(rows.values())):
        print(f"| {key} | " + " | ".join(str(rows[t][key]) for t, _ in columns) + " |")
    print()

    loads = [r for r in tim if r.get("event") == "load"]
    if loads:
        print("## Model load in the bench process\n")
        for r in loads:
            print(f"- {r['tag']}: {r['model']} in {r['seconds']} s")


if __name__ == "__main__":
    main()
