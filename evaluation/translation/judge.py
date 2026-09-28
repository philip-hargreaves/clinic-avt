"""Blind judge tasks for the translation study, and their scoring.

    python judge.py tasks        one task per sampled sheet and language, translations under random letters
    python judge.py plant        validation tasks: reference translations with known errors planted
    python judge.py score        MQM penalty, critical errors and adequacy per system and language
    python judge.py validity     how often the judge caught each planted error, per language
    python judge.py repeat       agreement between the first pass and a blind repeat in verdicts-repeat/
    python judge.py llm-tasks    NLLB against the note model, two letters per task, six of the same sheets
    python judge.py llm-score

Tasks are written to results/judge/<kind>/tasks/<task>.md and the letter keys to .../keys/<task>.json,
which the judge never sees. A verdict is the judge's JSON saved as .../verdicts/<task>.json.
"""

import json
import os
import random
import re
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import read_json, read_jsonl  # noqa: E402
from common.stats import mean_interval  # noqa: E402
from study import LANGUAGES, LOW_RESOURCE, REFERENCE, ROOT  # noqa: E402

SHEETS_JUDGED = 12
WEIGHT = {"critical": 25, "major": 5, "minor": 1}
LETTERS = "ABCDEFGHIJ"


def penalty(errors: list, words: int) -> float:
    """MQM penalty per 100 source words."""
    return sum(WEIGHT.get(e.get("severity"), 0) for e in errors) * 100 / words


def folder(kind: str, part: str) -> Path:
    path = ROOT / "results" / "judge" / kind / part
    path.mkdir(parents=True, exist_ok=True)
    return path


def write_task(kind: str, task: str, language: str, items: list):
    """Items are (source, [(system, text)]). Only the key file maps letters to systems."""
    rng = random.Random(task)
    body, key = [f"TARGET LANGUAGE: {language}", ""], {}
    for number, (source, entries) in enumerate(items, 1):
        rng.shuffle(entries)
        body += [f"ITEM {number}", "", "ENGLISH SOURCE", "", source, ""]
        for letter, (_, text) in zip(LETTERS, entries):
            body += [f"TRANSLATION {number}{letter}", "", text, ""]
        key[str(number)] = {"words": len(source.split()),
                            "letters": {letter: what for letter, (what, _) in zip(LETTERS, entries)}}
    (folder(kind, "tasks") / f"{task}.md").write_text("\n".join(body), encoding="utf-8", newline="\n")
    json.dump({"language": language, "items": key},
              open(folder(kind, "keys") / f"{task}.json", "w", encoding="utf-8"), indent=1)


SEQ2SEQ = ("nllb-600m-int8", "m2m100-1.2b-int8", "m2m100-418m-int8", "small100-int8")
LLM_ROW = ("nllb-600m-int8", "qwen3.5-4b-int4")


def tasks(kind="sheets", names=SEQ2SEQ, count=SHEETS_JUDGED):
    """One task per sheet and language. The llm kind pairs NLLB with the note model over the
    first sheets of the same sample."""
    systems = {}
    for name in names:
        path = ROOT / "results" / "sheets" / f"{name}.jsonl"
        systems[name] = {(r["id"], r["language"]): r for r in read_jsonl(path)}
    sheets = sorted({k[0] for k in systems[REFERENCE]})
    chosen = sorted(random.Random(7).sample(sheets, SHEETS_JUDGED))[:count]
    written = 0
    for sheet in chosen:
        for language in LANGUAGES:
            entries = [(name, rows[(sheet, language)]["translation"]) for name, rows in systems.items()
                       if (sheet, language) in rows]
            if len(entries) < len(systems):
                continue
            source = systems[REFERENCE][(sheet, language)]["source"]
            write_task(kind, f"{sheet}__{language}", language, [(source, entries)])
            written += 1
    print(written, "tasks over", len(chosen), "sheets and", len(systems), "systems")


NATIVE_ZERO = {"Urdu": 0x06F0, "Arabic": 0x0660, "Bengali": 0x09E6, "Gujarati": 0x0AE6, "Punjabi": 0x0A66}


def change_number(text: str, language: str):
    """The first number becomes another, in ASCII or in the language's own digits."""
    zero = NATIVE_ZERO.get(language)
    pattern = r"[0-9]+" + (f"|[{chr(zero)}-{chr(zero + 9)}]+" if zero else "")
    match = re.search(pattern, text)
    if not match:
        return None
    old = match.group(0)
    base = zero if zero and ord(old[0]) >= zero else ord("0")
    value = int("".join(str(ord(c) - base) for c in old))
    new = "".join(chr(base + int(d)) for d in str(value * 3 + 7))
    return text[:match.start()] + new + text[match.end():]


def flip_negation(source: str):
    if " not " in source:
        return source.replace(" not ", " ", 1)
    for verb in (" is ", " are ", " was ", " were ", " can ", " will ", " should ", " has ", " have "):
        if verb in source:
            return source.replace(verb, verb + "not ", 1)
    return None


def references(language: str) -> list[tuple[str, str]]:
    """(english, reference) pairs from FLORES-200, whose references were checked by hand. TICO-19
    has misaligned rows, which would make an untouched reference look wrong."""
    from translate import FLORES
    root = ROOT / "data" / "flores200_dataset" / "devtest"
    english = open(root / "eng_Latn.devtest", encoding="utf-8").read().splitlines()
    target = open(root / f"{FLORES[language]}.devtest", encoding="utf-8").read().splitlines()
    # Past the first 300, which the reference metrics use
    return [(e, t) for e, t in list(zip(english, target))[300:] if 6 <= len(e.split()) <= 30]


PARAGRAPHS = 4
KINDS = ["control", "omission", "addition", "number"]


def plant():
    """Per language four judge calls. Each holds one variant of every paragraph, so no call shows two
    versions of the same text, and two calls also hold a reference whose source was negated."""
    count = 0
    for language in LANGUAGES:
        pairs = references(language)
        rng = random.Random(language)
        rng.shuffle(pairs)
        numbered = [p for p in pairs if re.search(r"[0-9]", p[0]) and change_number(p[1], language)]
        plain = [p for p in pairs if p not in numbered]
        variants = []
        for _ in range(PARAGRAPHS):
            # Three sentences stand in for a short sheet. The middle one carries a number
            block = [plain.pop(), numbered.pop(), plain.pop()]
            source = " ".join(e for e, _ in block)
            target = [t for _, t in block]
            stray = plain.pop()[1]
            variants.append((source, {
                "control": " ".join(target),
                "omission": " ".join([target[0], target[1]]),
                "addition": " ".join([target[0], stray, target[1], target[2]]),
                "number": " ".join([target[0], change_number(target[1], language), target[2]])}))
        flips = [p for p in plain if flip_negation(p[0])][:2]
        for call in range(len(KINDS)):
            items = []
            for n, (source, texts) in enumerate(variants):
                kind = KINDS[(call + n) % len(KINDS)]
                items.append((source, [(kind, texts[kind])]))
            if call < len(flips):
                # Negating the source makes the untouched reference wrong
                english, reference = flips[call]
                items.append((flip_negation(english), [("negation", reference)]))
            rng.shuffle(items)
            write_task("plant", f"{language}-{call}", language, items)
            count += 1
    print(count, "validation tasks")


def verdicts(kind: str):
    for path in sorted(folder(kind, "verdicts").glob("*.json")):
        key = read_json(folder(kind, "keys") / path.name)
        try:
            verdict = read_json(path)
        except json.JSONDecodeError:
            print("unreadable verdict:", path.name)
            continue
        yield path.stem, key, verdict


def score(kind="sheets"):
    rows = defaultdict(dict)  # (system, language) -> task -> measures
    confidence = defaultdict(list)
    for task, key, verdict in verdicts(kind):
        confidence[key["language"]].append(verdict.get("judge_confidence", "?"))
        item = key["items"]["1"]
        for letter, system in item["letters"].items():
            judged = verdict["items"].get("1", {}).get(letter)
            if judged is None:
                continue
            errors = judged.get("errors", [])
            rows[(system, key["language"])][task.split("__")[0]] = {
                "penalty": penalty(errors, item["words"]),
                "critical": sum(e.get("severity") == "critical" for e in errors),
                "adequacy": judged.get("adequacy", 0), "fluency": judged.get("fluency", 0)}
    rng = np.random.default_rng(7)
    print(f"{'system':<20} {'language':<10}  MQM/100w  critical  adequacy  fluency   n"
          f" | penalty minus {REFERENCE} (95%)")
    table = {}
    for (system, language), by_task in sorted(rows.items()):
        names = sorted(by_task)

        def mean(field):
            return float(np.mean([by_task[t][field] for t in names]))

        entry = {"penalty": mean("penalty"), "critical": int(sum(by_task[t]["critical"] for t in names)),
                 "adequacy": mean("adequacy"), "fluency": mean("fluency"), "n": len(names)}
        interval = ""
        base = rows.get((REFERENCE, language), {})
        shared = [t for t in names if t in base]
        if system != REFERENCE and shared:
            delta = np.array([by_task[t]["penalty"] - base[t]["penalty"] for t in shared])
            low, high = mean_interval(rng, delta)
            entry["delta"] = [float(low), float(high)]
            interval = f"{low:+.1f} to {high:+.1f}"
        table[f"{system}|{language}"] = entry
        print(f"{system:<20} {language:<10}  {entry['penalty']:8.1f}  {entry['critical']:8d}  "
              f"{entry['adequacy']:8.1f}  {entry['fluency']:7.1f}  {len(names):3d} | {interval}")
    print()
    for system in sorted({s for s, _ in rows}):
        for label, group in (("low-resource", LOW_RESOURCE), ("all", LANGUAGES)):
            picked = [table[f"{system}|{language}"] for language in group if f"{system}|{language}" in table]
            if picked:
                mean_penalty = np.mean([p["penalty"] for p in picked])
                critical = sum(p["critical"] for p in picked)
                adequacy = np.mean([p["adequacy"] for p in picked])
                print(f"{system:<20} {label:<13} MQM/100w {mean_penalty:6.1f}  critical {critical:4d}  "
                      f"adequacy {adequacy:5.1f}")
    print()
    for language, levels in confidence.items():
        counts = ", ".join(f"{level} {levels.count(level)}" for level in sorted(set(levels)))
        print(f"judge confidence, {language}: {counts}")
    json.dump(table, open(ROOT / "results" / "judge" / kind / "scores.json", "w", encoding="utf-8"), indent=1)


# A planted error counts as caught when the judge marks a fitting category at major or critical
FITS = {"omission": {"omission"}, "addition": {"addition", "mistranslation"},
        "number": {"number", "mistranslation"}, "negation": {"negation", "mistranslation"}}


def validity():
    """A human reference is not flawless, so an untouched one may fairly draw major or minor marks.
    What it must not draw is a critical one."""
    caught = defaultdict(lambda: [0, 0])
    controls = defaultdict(lambda: [0, 0, 0])  # critical, major, shown
    for _, key, verdict in verdicts("plant"):
        for number, item in key["items"].items():
            (letter, planted), = item["letters"].items()
            errors = verdict["items"].get(number, {}).get(letter, {}).get("errors", [])
            serious = [e for e in errors if e.get("severity") in ("critical", "major")]
            if planted == "control":
                tally = controls[key["language"]]
                tally[0] += any(e.get("severity") == "critical" for e in errors)
                tally[1] += bool(serious)
                tally[2] += 1
            else:
                tally = caught[(key["language"], planted)]
                tally[0] += any(e.get("category") in FITS[planted] for e in serious)
                tally[1] += 1
    kinds = ["omission", "addition", "number", "negation"]
    print(f"{'language':<10} " + " ".join(f"{k:>10}" for k in kinds)
          + "   untouched references: critical, major or worse, shown")
    for language in LANGUAGES:
        cells = " ".join(f"{caught[(language, k)][0]:>6}/{caught[(language, k)][1]:<3}" for k in kinds)
        critical, major, shown = controls[language]
        print(f"{language:<10} {cells}   {critical}, {major}, {shown}")


def repeat():
    """Ten tasks (seed 11) judged a second time, both passes scored per translation."""
    first, second, crit1, crit2, adq1, adq2, best = [], [], [], [], [], [], 0
    for path in sorted(folder("sheets", "verdicts-repeat").glob("*.json")):
        item = read_json(folder("sheets", "keys") / path.name)["items"]["1"]
        a = read_json(folder("sheets", "verdicts") / path.name)["items"]["1"]
        b = read_json(path)["items"]["1"]

        def scored(verdict, letter):
            return penalty(verdict[letter]["errors"], item["words"])

        def critical(verdict, letter):
            return sum(e.get("severity") == "critical" for e in verdict[letter]["errors"])

        letters = item["letters"]
        best += min(letters, key=lambda x: scored(a, x)) == min(letters, key=lambda x: scored(b, x))
        for letter in letters:
            first.append(scored(a, letter))
            second.append(scored(b, letter))
            crit1.append(critical(a, letter))
            crit2.append(critical(b, letter))
            adq1.append(a[letter]["adequacy"])
            adq2.append(b[letter]["adequacy"])
    first, second, crit1, crit2, adq1, adq2 = map(np.array, (first, second, crit1, crit2, adq1, adq2))
    print("translations compared:", len(first))
    print("MQM penalty  Pearson %.3f  mean |diff| %.1f"
          % (np.corrcoef(first, second)[0, 1], np.mean(np.abs(first - second))))
    print("adequacy     Pearson %.3f  mean |diff| %.1f"
          % (np.corrcoef(adq1, adq2)[0, 1], np.mean(np.abs(adq1 - adq2))))
    same = int(((crit1 > 0) == (crit2 > 0)).sum())
    print("critical     Pearson %.3f  same zero/non-zero %d/%d  total %d vs %d"
          % (np.corrcoef(crit1, crit2)[0, 1], same, len(crit1), crit1.sum(), crit2.sum()))
    print("best system per task agrees: %d/%d" % (best, len(first) // 4))


if __name__ == "__main__":
    step = sys.argv[1] if len(sys.argv) > 1 else ""
    steps = {"tasks": tasks, "plant": plant, "score": score, "validity": validity, "repeat": repeat,
             "llm-tasks": lambda: tasks("llm", LLM_ROW, 6), "llm-score": lambda: score("llm")}
    steps.get(step, lambda: print(__doc__))()
