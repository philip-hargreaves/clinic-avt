"""One table per language from every measure the study saved, and the original eight's headline figures.

    python summarise.py

Reads the scores the other steps write under mt_root (score.py refs and integrity, judge.py score,
comet_qe.py) and the judge verdicts, so run those first. Writes results/summary/languages.txt and
languages.json, and prints the NLLB figures for the original eight languages beside the figures
first reported for them.
"""

import json
import os
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import read_json, read_jsonl  # noqa: E402
from judge import verdicts  # noqa: E402
from languages import LANGUAGES, ORIGINAL  # noqa: E402
from study import REFERENCE, ROOT  # noqa: E402

# The figures the original eight-language study reported for NLLB
REPORTED = {"flores_chrf": 48.2, "critical": 53}


def scores(*parts: str) -> dict:
    path = ROOT.joinpath("results", *parts)
    return read_json(path) if path.exists() else {}


def judged() -> dict:
    """Critical and major error counts per system and language, from the verdicts."""
    counts = defaultdict(lambda: {"critical": 0, "major": 0})
    for _, key, verdict in verdicts("sheets"):
        item = key["items"]["1"]
        for letter, system in item["letters"].items():
            errors = verdict["items"].get("1", {}).get(letter, {}).get("errors", [])
            entry = counts[f"{system}|{key['language']}"]
            entry["critical"] += sum(e.get("severity") == "critical" for e in errors)
            entry["major"] += sum(e.get("severity") == "major" for e in errors)
    return counts


def comet() -> dict:
    means = {}
    for path in sorted((ROOT / "results" / "comet").glob("*.jsonl")):
        by_language = defaultdict(list)
        for row in read_jsonl(path):
            if row["comet"] is not None:
                by_language[row["language"]].append(row["comet"])
        for language, values in by_language.items():
            means[f"{path.stem}|{language}"] = float(np.mean(values))
    return means


def main():
    flores, tico = scores("flores", "scores.json"), scores("tico", "scores.json")
    integrity, judge = scores("sheets", "integrity.json"), scores("judge", "sheets", "scores.json")
    errors, qe = judged(), comet()
    systems = sorted({key.split("|")[0] for table in (flores, integrity, judge) for key in table})
    header = (f"{'language':<22} {'system':<18} {'FLORES':>6} {'spBLEU':>6} {'TICO':>5} {'loops':>5} "
              f"{'faults':>6} {'crit':>4} {'major':>5} {'adeq':>5} {'COMET':>6}")
    lines, table = [header], {}

    def cell(value, width, digits=1):
        return f"{value:>{width}.{digits}f}" if isinstance(value, (int, float)) else f"{'-':>{width}}"

    for language in LANGUAGES:
        for system in systems:
            key = f"{system}|{language}"
            sheet = integrity.get(key, {})
            row = {"flores_chrf": flores.get(key, {}).get("chrf"), "flores_spbleu": flores.get(key, {}).get("spbleu"),
                   "tico_chrf": tico.get(key, {}).get("chrf"), "loops": sheet.get("loops"),
                   "faults": sum(sheet.get(k, 0) for k in ("numbers", "lines", "script", "empty"))
                   if sheet else None,
                   "critical": errors[key]["critical"] if key in errors else None,
                   "major": errors[key]["major"] if key in errors else None,
                   "adequacy": judge.get(key, {}).get("adequacy"), "comet_qe": qe.get(key)}
            if all(v is None for v in row.values()):
                continue
            table[key] = row
            lines.append(f"{language:<22} {system:<18} {cell(row['flores_chrf'], 6)} {cell(row['flores_spbleu'], 6)} "
                         f"{cell(row['tico_chrf'], 5)} {cell(row['loops'], 5, 0)} {cell(row['faults'], 6, 0)} "
                         f"{cell(row['critical'], 4, 0)} {cell(row['major'], 5, 0)} {cell(row['adequacy'], 5)} "
                         f"{cell(row['comet_qe'], 6, 3)}")
    out = ROOT / "results" / "summary"
    out.mkdir(parents=True, exist_ok=True)
    (out / "languages.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    json.dump(table, open(out / "languages.json", "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    print("\n".join(lines))

    # The original eight, measured as in the first study
    chrf = [flores[f"{REFERENCE}|{language}"]["chrf"] for language in ORIGINAL if f"{REFERENCE}|{language}" in flores]
    critical = sum(judge[f"{REFERENCE}|{language}"]["critical"] for language in ORIGINAL
                   if f"{REFERENCE}|{language}" in judge)
    measured = {"flores_chrf": round(float(np.mean(chrf)), 1) if chrf else None, "critical": critical}
    print(f"\n{REFERENCE}, original eight languages:")
    for name, expected in REPORTED.items():
        status = "matches" if measured[name] == expected else "DIFFERS"
        print(f"  {name:<12} {measured[name]}  (reported {expected}, {status})")


if __name__ == "__main__":
    main()
