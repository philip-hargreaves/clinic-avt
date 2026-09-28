"""Prompt-example leaks: phrases from the prompts' worked examples that turn up in outputs
without being in the source (transcript for notes, note for sheets and labels).

    python evaluation/summarisation/leaks.py [tag ...]   # default: tier-constrained tier-default tier-accuracy
"""
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402

NOTES = str(config.path("summarisation") / "notes")
TRANSCRIPTS = str(config.path("perf_loop") / "transcripts")
TAG = config.section("summarisation")["transcript_tag"]

# Phrase -> regex. Each came from a worked example in a prompt
PROBES = {
    "53-year-old": r"\b53[- ]year",
    "elbow": r"\belbow",
    "bursitis": r"\bbursitis",
    "septic arthritis": r"\bseptic",
    "peanut": r"\bpeanut",
    "warm to the touch": r"warm to the touch",
    "blood test forms": r"blood test forms?",
    "ibuprofen": r"\bibuprofen",
    "effusion": r"\beffusion",
    "raised his elbow": r"raised (his|her) elbow",
    "redness": r"\bredness",
    "400mg": r"\b400\s?mg",
}
GENDERED = r"\b(he|his|him|she|her|hers|male|female|man|woman|mr|mrs|ms|miss|sir|madam|gentleman|lady)\b"
LABEL_EXAMPLES = ["elbow swelling", "diarrhoea and vomiting", "medication review", "chest pain follow-up", "knee pain", "low mood", "asthma review"]


def transcript(cid):
    path = os.path.join(TRANSCRIPTS, f"{TAG}-{cid}_mixed.json")
    turns = json.load(open(path, encoding="utf-8"))["turns"]
    return "\n".join(t.get("text", "") for t in turns)


def read(path):
    return open(path, encoding="utf-8").read() if os.path.exists(path) else None


def leaks(text, source):
    found = []
    for name, rx in PROBES.items():
        if re.search(rx, text, re.I) and not re.search(rx, source, re.I):
            found.append(name)
    return found


NOTE_KINDS = ("concise", "detailed", "standard", "soap")


def main():
    prefixes = sys.argv[1:] or ["tier-constrained", "tier-default", "tier-accuracy"]
    for prefix in prefixes:
        # Sheets and titles are written from this note
        base = "concise" if os.path.isdir(os.path.join(NOTES, f"{prefix}-concise")) else "standard"
        # standard: the middle length of runs banked before the two lengths
        for kind in ["concise", "detailed", "standard", "soap", "sheet", "label"]:
            files = sorted(glob.glob(os.path.join(NOTES, f"{prefix}-{kind}", "*.md")))
            if not files:
                continue
            counts, n, refused, empty, pronoun, examples = {}, 0, 0, 0, 0, 0
            for f in files:
                cid = os.path.basename(f)[:-3]
                text = read(f)
                n += 1
                if not text.strip():
                    empty += 1
                    continue
                if text.startswith("NOT A CONSULTATION"):
                    refused += 1
                    continue
                note_kind = "soap" if kind == "soap" else base
                source = transcript(cid) if kind in NOTE_KINDS else (
                    read(os.path.join(NOTES, f"{prefix}-{note_kind}", f"{cid}.md")) or "")
                for name in leaks(text, source):
                    counts[name] = counts.get(name, 0) + 1
                if kind in NOTE_KINDS:
                    said = transcript(cid)
                    if re.search(GENDERED, text, re.I) and not re.search(GENDERED, said, re.I):
                        pronoun += 1
                if kind == "label" and text.strip().lower().strip('"') in LABEL_EXAMPLES \
                        and text.strip().lower().strip('"') not in source.lower():
                    examples += 1
            extra = f", guessed sex {pronoun}" if kind in NOTE_KINDS else ""
            extra += f", example title {examples}" if kind == "label" else ""
            listed = ", ".join(f"{k} {v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1])) or "none"
            print(f"{prefix}-{kind}: n={n} refused={refused} empty={empty}{extra}; leaks: {listed}")


if __name__ == "__main__":
    main()
