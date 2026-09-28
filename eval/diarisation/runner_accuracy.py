"""Diarisation accuracy of the engine against the research port it was built from.

Runs diar_eval_runner over every mixed PriMock track and scores attribution with the selection
bench's time-weighted rule: a gold segment is right when the slices overlapping it give more time
to its own role. The engine's clusters are anonymous here (roles come later in the pipeline), so
both sides go through the same oracle map: each cluster takes the gold role it overlaps most.
The reference is the research C++ port's per-slice output (reference/research-port-slices.txt,
96.94% with its own role rule). Passes when the two agree within 0.5 points.

    python eval/diarisation/runner_accuracy.py [--reference-only]
"""
import os
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from diarisation.runner import cluster_votes, consults, gold, overlap, run  # noqa: E402

REFERENCE = Path(__file__).parent / "reference" / "research-port-slices.txt"


def accuracy(slices, g):
    mapping = {c: 1 if v[1] >= v[0] else 0 for c, v in cluster_votes(slices, g).items()}
    num = den = 0.0
    for gs in g:
        seconds = {0: 0.0, 1: 0.0}
        for s, e, c in slices:
            o = overlap(gs["start"], gs["end"], s, e)
            if o > 0:
                seconds[mapping[c]] += o
        d = gs["end"] - gs["start"]
        right = (seconds[1] >= seconds[0]) == (gs["label"] == 1) if (seconds[0] or seconds[1]) else False
        num += d * right
        den += d
    return num, den


def main():
    reference_only = "--reference-only" in sys.argv
    research = defaultdict(list)
    for line in REFERENCE.read_text().splitlines():
        if line.strip():
            consult, s, e, pred = line.split()
            research[consult].append((float(s), float(e), int(pred)))
    totals = {"engine": [0.0, 0.0], "research": [0.0, 0.0]}
    rows = []
    for consult in consults():
        g = gold(consult)
        ra, rb = accuracy(research[consult], g)
        totals["research"][0] += ra
        totals["research"][1] += rb
        line = f"{consult}: research {100 * ra / rb:6.2f}%"
        if not reference_only:
            slices = [(float(a), float(b), int(c)) for a, b, c in
                      (x.split() for x in run(consult).splitlines() if x.strip())]
            a, b = accuracy(slices, g)
            totals["engine"][0] += a
            totals["engine"][1] += b
            rows.append((consult, 100 * a / b - 100 * ra / rb))
            line += f"  engine {100 * a / b:6.2f}%"
        print(line, flush=True)
    research_pct = 100 * totals["research"][0] / totals["research"][1]
    print(f"\nresearch reference: {research_pct:.2f}% (oracle-mapped, n={len(consults())})")
    if reference_only:
        return
    engine_pct = 100 * totals["engine"][0] / totals["engine"][1]
    worst = min(rows, key=lambda r: r[1])
    print(f"engine:             {engine_pct:.2f}%  delta {engine_pct - research_pct:+.2f} pt, "
          f"worst consult {worst[0]} {worst[1]:+.2f} pt")
    ok = abs(engine_pct - research_pct) < 0.5
    print(f"[{'PASS' if ok else 'FAIL'}] the engine reproduces the research pipeline")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
