"""BERTScore on the 100 MTS-Dialog validation pairs, in the previous system's exact configuration,
for continuity with its reported numbers. Not a quality measure for selection: the judge is.

  generate  summaries with the previous system's prompt as the system turn and the dialogue as
            the user turn, sampled at temperature 0.3, 1024 tokens -> bertscore/<model>.csv
  score     bert_score.score(candidates, references, lang="en", batch_size=32), no baseline
            rescaling, per CSV in a folder -> bertscore/scores.json

    python evaluation/summarisation/bertscore.py generate qwen3.5-9b-int4-ov
    python evaluation/summarisation/bertscore.py score [--dir <folder of csv>]
"""
import argparse
import csv
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import write_json  # noqa: E402

PROMPT = (
    "\n    You are an expert medical scribe for the NHS in England. Your goal is to convert raw "
    "doctor-patient transcripts into a clinical summary\n\n    ### Instructions:\n    1. Use "
    "standard medical terminology and abbreviations where appropriate\n    2. Do not make up any "
    "dosages or facts that are not present in this conversation\n    3. Do not infer dates unless "
    "specifically stated. Write dates in the format DD/MM/YYYY\n    4. Provide only the summary, do "
    "not include any new lines or quotes\n"
)


def pairs():
    out = []
    for d in json.loads(config.path("mts_dialog").read_text(encoding="utf-8")):
        text = d["text"]
        out.append((text.split("### Input:")[1].split("### Response:")[0].strip(),
                    text.split("### Response:")[1].strip()))
    return out


def generate(name, limit):
    from common.llm import NoteModel
    model = NoteModel(name)
    path = config.out("summarisation", "bertscore") / f"{name}.csv"
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, quoting=csv.QUOTE_ALL)
        w.writerow(["ID", "ground_truth", "response", "time_taken"])
        for i, (dialogue, ref) in enumerate(pairs()[: limit or None]):
            t0 = time.time()
            text, _ = model.generate(dialogue, 1024, system=PROMPT, temperature=0.3)
            w.writerow([i, ref, text, round(time.time() - t0, 2)])
            f.flush()
    print(f"-> {path}")


def score(folder):
    import pandas as pd
    from bert_score import score as bert_score
    rows = []
    for path in sorted(folder.glob("*.csv")):
        if path.stem == "benchmarks":
            continue
        df = pd.read_csv(path)
        p, r, f1 = bert_score(df["response"].astype(str).tolist(), df["ground_truth"].astype(str).tolist(),
                              lang="en", batch_size=32, verbose=False)
        rows.append({"model": path.stem, "n": len(df), "F1": f1.mean().item(), "P": p.mean().item(),
                     "R": r.mean().item()})
    print(f"{'model':40} {'n':>4} {'F1':>8} {'P':>8} {'R':>8}")
    for row in sorted(rows, key=lambda x: -x["F1"]):
        print(f"{row['model']:40} {row['n']:>4} {row['F1']:8.4f} {row['P']:8.4f} {row['R']:8.4f}")
    write_json(config.out("summarisation", "bertscore") / "scores.json", rows)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("model")
    g.add_argument("--limit", type=int, default=0)
    s = sub.add_parser("score")
    s.add_argument("--dir", help="folder of response CSVs (default build/evaluation/summarisation/bertscore)")
    args = ap.parse_args()
    if args.cmd == "generate":
        generate(args.model, args.limit)
    else:
        from pathlib import Path
        score(Path(args.dir) if args.dir else config.out("summarisation", "bertscore"))


if __name__ == "__main__":
    main()
