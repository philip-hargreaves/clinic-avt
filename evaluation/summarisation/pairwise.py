"""Pairwise note judge. For each grid arm, asks which draft a GP would rather start from, the arm's
note or the shipped setting's (shipped prompt, greedy). Each pair gets one isolated judge with the
frozen prompt (pairwise-prompt.md) and schema (pairwise-schema.json), and sees the two notes in a
random order fixed by the pair.

  tasks    one task per (arm, consultation) under judge/pairwise/tasks/<arm>/<cid>.md, plus the
           hidden order in judge/pairwise/order.json; first draw only, checklist consultations only
           unless --all
  pending  tasks without an answer in judge/pairwise/out/<arm>/<cid>.json
  score    win, tie and loss rates per arm against the shipped setting -> pairwise.md / pairwise.json

    python evaluation/summarisation/pairwise.py tasks --tier default
    python evaluation/summarisation/pairwise.py pending
    python evaluation/summarisation/pairwise.py score
"""
import argparse
import hashlib
import json
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import read_json  # noqa: E402
from summarisation import judge  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PROMPTS = ["shipped", "simple", "minimal"]
TEMPS = ["greedy", "0.3", "0.7"]


def root():
    return judge.data() / "judge" / "pairwise"


def prompt():
    text = open(os.path.join(HERE, "pairwise-prompt.md"), encoding="utf-8").read()
    return text.split("---", 1)[1].strip()


def baseline(tier):
    return f"grid-{tier}-shipped-greedy-d1"


def write_tasks(tier, everything):
    manifest = {m["consult_id"]: m for m in read_json(judge.data() / "prep" / "manifest.json")}
    order_path = root() / "order.json"
    order = read_json(order_path) if order_path.exists() else {}
    base = judge.data() / "notes" / baseline(tier)
    count = 0
    for p in PROMPTS:
        for t in TEMPS:
            arm = f"grid-{tier}-{p}-{t}-d1"
            if arm == baseline(tier):
                continue
            out = root() / "tasks" / arm
            out.mkdir(parents=True, exist_ok=True)
            for path in sorted((judge.data() / "notes" / arm).glob("*.md")):
                cid = path.stem
                if cid not in manifest or not (everything or manifest[cid]["checklist"]):
                    continue
                ref = base / f"{cid}.md"
                text = judge.transcript(cid, "sealed", manifest)
                if not ref.exists() or text is None:
                    continue
                arm_note, base_note = path.read_text(encoding="utf-8").strip(), ref.read_text(encoding="utf-8").strip()
                # Arm shown as A on an even hash of the pair, B otherwise
                arm_first = int(hashlib.sha256(f"{arm}/{cid}".encode()).hexdigest(), 16) % 2 == 0
                a, b = (arm_note, base_note) if arm_first else (base_note, arm_note)
                order[f"{arm}/{cid}"] = "A" if arm_first else "B"
                (out / f"{cid}.md").write_text(
                    prompt() + "\n\n---\n\nTRANSCRIPT:\n" + text + "\n\n---\n\nNOTE A:\n" + a +
                    "\n\n---\n\nNOTE B:\n" + b + "\n", encoding="utf-8")
                count += 1
    order_path.parent.mkdir(parents=True, exist_ok=True)
    order_path.write_text(json.dumps(order, indent=1), encoding="utf-8")
    print(f"{count} pairwise tasks for {tier}")


def pending():
    out = []
    for task in sorted((root() / "tasks").glob("*/*.md")):
        answer = root() / "out" / task.parent.name / f"{task.stem}.json"
        if not answer.exists():
            out.append((task.parent.name, task.stem, task))
    return out


def score():
    order = read_json(root() / "order.json")
    results = {}
    for path in sorted((root() / "out").glob("*/*.json")):
        arm, cid = path.parent.name, path.stem
        try:
            raw = path.read_text(encoding="utf-8").strip()
            raw = raw[raw.find("{"):raw.rfind("}") + 1]
            answer = json.loads(raw)
            assert answer["preferred"] in ("A", "B", "tie")
        except (ValueError, AssertionError, KeyError) as e:
            print(f"BAD {arm} {cid}: {e}")
            continue
        side = order[f"{arm}/{cid}"]
        outcome = "tie" if answer["preferred"] == "tie" else ("win" if answer["preferred"] == side else "loss")
        results.setdefault(arm, Counter())[outcome] += 1
    lines = ["# Pairwise judge: each arm against the shipped prompt at greedy\n",
             "| Arm | Pairs | Arm preferred | Tie | Shipped preferred |", "|---|---|---|---|---|"]
    table = {}
    for arm, c in sorted(results.items()):
        n = sum(c.values())
        table[arm] = {"n": n, **{k: c[k] for k in ("win", "tie", "loss")}}
        lines.append(f"| {arm} | {n} | {c['win']} ({100 * c['win'] / n:.0f}%) | {c['tie']} | "
                     f"{c['loss']} ({100 * c['loss'] / n:.0f}%) |")
    (judge.data() / "pairwise.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (judge.data() / "pairwise.json").write_text(json.dumps(table, indent=1), encoding="utf-8")
    print("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tasks")
    t.add_argument("--tier", required=True, choices=["constrained", "default", "accuracy"])
    t.add_argument("--all", action="store_true", help="all 57 consultations, not only the 20 with checklists")
    sub.add_parser("pending")
    sub.add_parser("score")
    args = ap.parse_args()
    if args.cmd == "tasks":
        write_tasks(args.tier, args.all)
    elif args.cmd == "pending":
        rows = pending()
        for arm, cid, task in rows:
            print(f"{arm}\t{cid}\t{task}")
        print(f"{len(rows)} pending", file=sys.stderr)
    else:
        score()


if __name__ == "__main__":
    main()
