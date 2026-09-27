"""Scores the candidates' translations.

    python score.py refs <flores|tico>     chrF++ and spBLEU per system and language, and each system's
                                           sentence-level chrF++ difference from the reference system
    python score.py integrity              deterministic checks on the patient sheets
    python score.py cost                   seconds per sheet

Reads results/<set>/<system>.jsonl under MT_ROOT and prints tables. Writes the same as
results/<set>/scores.json.
"""

import json
import re
import sys
import unicodedata
from collections import defaultdict

import numpy as np

from common import LOW_RESOURCE, REFERENCE, ROOT, bootstrap_interval, read_jsonl

SCRIPT = {"Urdu": "ARABIC", "Arabic": "ARABIC", "Punjabi": "GURMUKHI", "Bengali": "BENGALI",
          "Gujarati": "GUJARATI", "Polish": "LATIN", "Romanian": "LATIN", "Somali": "LATIN"}


def load(set_name: str) -> dict:
    rows = defaultdict(dict)
    for path in sorted((ROOT / "results" / set_name).glob("*.jsonl")):
        for row in read_jsonl(path):
            rows[path.stem][(row["id"], row["language"])] = row
    return rows


def refs(set_name: str):
    from sacrebleu.metrics import BLEU, CHRF

    chrf, bleu = CHRF(word_order=2), BLEU(tokenize="flores200")
    systems = load(set_name)
    languages = sorted({k[1] for rows in systems.values() for k in rows})
    rng, table = np.random.default_rng(7), {}
    print(f"{'system':<20} {'language':<10} chrF++  spBLEU   n | chrF++ minus {REFERENCE} (95%)")
    for system, rows in systems.items():
        for language in languages:
            keys = sorted(k for k in rows if k[1] == language and k in systems.get(REFERENCE, rows))
            if not keys:
                continue
            hyp = [rows[k]["translation"] for k in keys]
            ref = [rows[k]["reference"] for k in keys]
            entry = {"chrf": chrf.corpus_score(hyp, [ref]).score,
                     "spbleu": bleu.corpus_score(hyp, [ref]).score, "n": len(keys)}
            interval = ""
            if system != REFERENCE and REFERENCE in systems:
                base = systems[REFERENCE]
                mine = np.array([chrf.sentence_score(h, [r]).score for h, r in zip(hyp, ref)])
                theirs = np.array([chrf.sentence_score(base[k]["translation"], [base[k]["reference"]]).score
                                   for k in keys])
                low, high = bootstrap_interval(rng, mine - theirs)
                entry["delta"] = [round(float(low), 2), round(float(high), 2)]
                interval = f"{low:+.1f} to {high:+.1f}"
            table[f"{system}|{language}"] = entry
            print(f"{system:<20} {language:<10} {entry['chrf']:5.1f}  {entry['spbleu']:5.1f}  "
                  f"{len(keys):4d} | {interval}")
    print()
    for system in systems:
        for label, group in (("low-resource", LOW_RESOURCE), ("all", languages)):
            scores = [table[f"{system}|{language}"]["chrf"] for language in group
                      if f"{system}|{language}" in table]
            if scores:
                print(f"{system:<20} mean chrF++ {label:<13} {np.mean(scores):5.1f} "
                      f"over {len(scores)} languages")
    json.dump(table, open(ROOT / "results" / set_name / "scores.json", "w", encoding="utf-8"), indent=1)


def digits(text: str) -> list[str]:
    """Every run of digits, native numerals read as ASCII."""
    plain = "".join(str(unicodedata.digit(c)) if c.isdigit() and not c.isascii() else c for c in text)
    return sorted(re.findall(r"\d+(?:[.,]\d+)?", plain))


def in_script(text: str, script: str) -> float:
    letters = [c for c in text if c.isalpha()]
    if not letters:
        return 0.0
    return sum(script in unicodedata.name(c, "") for c in letters) / len(letters)


def loops(text: str) -> bool:
    words = text.split()
    for size in (1, 2, 3):
        for i in range(len(words) - size * 4 + 1):
            chunk = words[i:i + size]
            if all(words[i + size * k:i + size * (k + 1)] == chunk for k in range(1, 4)):
                return True
    return False


def integrity():
    systems = load("sheets")
    print(f"{'system':<20} {'language':<10} sheets  numbers  lines  script  loops  empty  short  long")
    table = {}
    for system, rows in systems.items():
        for language in sorted({k[1] for k in rows}):
            group = [r for k, r in rows.items() if k[1] == language]
            counts = defaultdict(int)
            for r in group:
                source, target = r["source"], r["translation"]
                counts["numbers"] += digits(source) != digits(target)
                counts["lines"] += len(source.split("\n")) != len(target.split("\n"))
                counts["script"] += in_script(target, SCRIPT[language]) < 0.85
                counts["loops"] += loops(target)
                pairs = list(zip(source.split("\n"), target.split("\n")))
                counts["empty"] += any(s.strip() and not t.strip() for s, t in pairs)
                ratio = len(target) / max(1, len(source))
                counts["short"] += ratio < 0.5
                counts["long"] += ratio > 2.5
            table[f"{system}|{language}"] = {"sheets": len(group), **counts}
            print(f"{system:<20} {language:<10} {len(group):6d} " + " ".join(
                f"{counts[k]:6d}" for k in ("numbers", "lines", "script", "loops", "empty", "short", "long")))
    json.dump(table, open(ROOT / "results" / "sheets" / "integrity.json", "w", encoding="utf-8"), indent=1)


def cost():
    for system, rows in load("sheets").items():
        seconds = np.array([r["seconds"] for r in rows.values()])
        print(f"{system:<20} per sheet: median {np.median(seconds):5.1f} s, "
              f"95th {np.percentile(seconds, 95):5.1f} s, n {len(seconds)}")


if __name__ == "__main__":
    step = sys.argv[1] if len(sys.argv) > 1 else ""
    if step == "refs":
        refs(sys.argv[2])
    elif step == "integrity":
        integrity()
    elif step == "cost":
        cost()
    else:
        print(__doc__)
