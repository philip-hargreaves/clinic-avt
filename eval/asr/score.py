"""Score saved hypotheses: surface metrics (WER, content WER, CER, raw WER) and, with
--clinical, the clinical lens (concept, drug and dose recall, negations kept).

Per input -> scores/<ir>__<form>.jsonl; per export and form -> scores/aggregate.json; the
mixed-form ranking to stdout. --hyp reads another hypothesis folder, such as a banked run.

    python eval/asr/score.py [--form mixed] [--clinical] [--hyp <folder>]
"""
import argparse
import json
import os
import statistics
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from asr import metrics, references  # noqa: E402
from asr.normalise import normalize, normalize_raw  # noqa: E402


def r(x, n=4):
    return round(x, n) if x is not None else None


def score(hyp_path, ir, consult, form, clinical):
    ref = references.load(consult)
    hyp_j = json.loads(hyp_path.read_text(encoding="utf-8"))
    hyp = hyp_j["text"]
    ref_text = ref["merged_ref"] if form == "mixed" else ref[f"{form}_ref"]
    ref_segs = ref if form == "mixed" else {"doctor": [], "patient": [], form: ref[form]}
    std, con = metrics.standard_and_content(ref_segs, hyp)
    cer = metrics.error_rate(normalize(ref_text), normalize(hyp), char=True)
    raw = metrics.error_rate(normalize_raw(ref_text), normalize_raw(hyp))
    row = {"ir": ir, "stem": consult, "form": form,
           "audio_s": hyp_j.get("audio_s"), "proc_s": hyp_j.get("proc_s"), "rtfx": hyp_j.get("rtfx"),
           "wer": {"standard": r(std["err"]), "content": r(con["err"]), "raw": r(raw["err"]), "cer": r(cer["err"])},
           "wer_detail": {"N": std["N"], "S": std["S"], "D": std["D"], "I": std["I"],
                          "content_N": con["N"], "filler_discounted": con["dropped"]}}
    if clinical:
        from asr import clinical as lens
        c = lens.analyze(ref_text, hyp)
        row["clinical"] = {"concept_recall": r(c["concept_recall"], 3), "drug_recall": r(c["drug_recall"], 3),
                           "neg_preserved": r(c["neg_preserved"], 3), "dose_recall": r(c["dose_recall"], 3),
                           "n_concepts": c["n_concepts"], "n_drugs": c["n_drugs"], "n_neg": c["n_neg"],
                           "missed": [x["text"] for x in c["rows"] if not x["present"]],
                           "neg_flips": [x["text"] for x in c["rows"] if x["neg_ok"] is False]}
    return row


def median(xs):
    xs = [x for x in xs if x is not None]
    return round(statistics.median(xs), 4) if xs else None


def mean(xs):
    xs = [x for x in xs if x is not None]
    return round(sum(xs) / len(xs), 3) if xs else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--form", default="", help="only this form: mixed, doctor or patient")
    ap.add_argument("--clinical", action="store_true", help="add the clinical lens (scispaCy)")
    ap.add_argument("--hyp", help="hypothesis folder (default build/eval/asr/hyp)")
    args = ap.parse_args()
    hyp_root = Path(args.hyp) if args.hyp else config.out("asr", "hyp")
    out = config.out("asr", "scores")
    aggregate = []
    for ir_dir in sorted(p for p in hyp_root.iterdir() if p.is_dir()):
        summary_path = ir_dir / "_summary.json"
        summary = json.loads(summary_path.read_text()) if summary_path.exists() else {}
        by_form = {}
        for f in sorted(ir_dir.glob("*.json")):
            if f.name == "_summary.json":
                continue
            consult, form = f.stem.rsplit("_", 1)
            if not args.form or form == args.form:
                by_form.setdefault(form, []).append((consult, f))
        for form, items in by_form.items():
            rows = [score(f, ir_dir.name, consult, form, args.clinical) for consult, f in items]
            (out / f"{ir_dir.name}__{form}.jsonl").write_text(
                "\n".join(json.dumps(row, ensure_ascii=False) for row in rows), encoding="utf-8")
            rec = {"ir": ir_dir.name, "form": form, "n": len(rows),
                   "wer_std": median([row["wer"]["standard"] for row in rows]),
                   "wer_content": median([row["wer"]["content"] for row in rows]),
                   "cer": median([row["wer"]["cer"] for row in rows])}
            if args.clinical:
                rec.update({"concept_recall": mean([row["clinical"]["concept_recall"] for row in rows]),
                            "drug_recall": mean([row["clinical"]["drug_recall"] for row in rows]),
                            "neg_preserved": mean([row["clinical"]["neg_preserved"] for row in rows]),
                            "neg_flips": sum(len(row["clinical"]["neg_flips"]) for row in rows)})
            rec.update({"rtfx": median([row.get("rtfx") for row in rows]) or summary.get("median_rtfx"),
                        "peak_mb": summary.get("peak_mb"), "device": summary.get("device")})
            aggregate.append(rec)
    (out / "aggregate.json").write_text(json.dumps(aggregate, indent=2), encoding="utf-8")

    def pct(x):
        return f"{x * 100:.1f}" if x is not None else "-"
    print(f"{'export':38s} {'n':>3} {'WER':>6} {'WERc':>6} {'CER':>6} {'RTFx':>6}")
    for rec in sorted((a for a in aggregate if a["form"] == "mixed"), key=lambda a: a["wer_std"] or 9):
        print(f"{rec['ir']:38s} {rec['n']:>3} {pct(rec['wer_std']):>6} {pct(rec['wer_content']):>6} "
              f"{pct(rec['cer']):>6} {str(rec['rtfx'] or '-'):>6}")
    print(f"-> {out}")


if __name__ == "__main__":
    main()
