"""Write a review table for one gold set: each row's labels beside what a run retrieved.

  python review.py <run-dir-name> primock|synthetic   -> rag/results/<set>-review.md
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import log, read_jsonl  # noqa: E402
from harness import GOLD, RESULTS, latest_chunks  # noqa: E402

SETS = {
    "primock": (GOLD / "primock-statements" / "statements.jsonl", "consult"),
    "synthetic": (GOLD / "synthetic-statements" / "statements.jsonl", "scenario"),
}


def clip(text: str, n: int) -> str:
    return re.sub(r"\s+", " ", text)[:n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("set", choices=sorted(SETS))
    ap.add_argument("--mode", default="sentence")
    args = ap.parse_args()

    path, origin = SETS[args.set]
    rows = read_jsonl(path)
    chunks = {c["id"]: c["text"] for c in read_jsonl(latest_chunks())}
    top = {}
    for r in read_jsonl(RESULTS / args.run / "per_query.jsonl"):
        mode = "note" if r["mode"] == "note" else args.mode
        if r["set"] == args.set and r["mode"] == mode and r["hybrid"] == "off" and r["reranker"] == "none":
            top[r["qid"]] = r["top"][:5]

    out = [f"# {args.set} labels", "",
           f"Run {args.run}. A tick marks a labelled recommendation; a note marks a judgement call.", "",
           f"| qid | {origin} | status | text | expected | why | retrieved top 5 |", "|---|---|---|---|---|---|---|"]
    for r in rows:
        exp = "<br>".join(f"**{i}** {clip(chunks[i], 130)}" for i in r["expected_ids"]) or "*none expected*"
        got = "<br>".join(f"{'✓ ' if i in r['expected_ids'] else ''}{i} ({s:.2f}) {clip(chunks.get(i, ''), 90)}"
                          for i, s in top.get(r["qid"], []))
        why = r["rationale"] + (f"<br>*note: {r['note']}*" if r.get("note") else "")
        out.append(f"| {r['qid']} | {r[origin]} | {r['status']} | {r['text']} | {exp} | {why} | {got} |")
    dest = RESULTS / f"{args.set}-review.md"
    dest.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    log(f"{len(rows)} rows -> {dest}")


if __name__ == "__main__":
    main()
