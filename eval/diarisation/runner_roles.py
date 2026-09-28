"""Role naming of the engine over the 57 PriMock consultations (diar_eval_runner --roles: ASR,
diarisation, text assignment, cold-start naming).

  cold start  named right / abstained / named wrong against gold. The bar is zero wrong: an
              abstention is the design working, a confident wrong name corrupts a record.
  anchor      clinicians found by grouping each consult's true-doctor voiceprint (cosine above
              0.68, the research grouping rule); for each consult of a recurring clinician the
              anchor is the mean of the other consults' doctor prints, and the nearer of the two
              dominant clusters must be the true doctor. Leave-one-out.

    python eval/diarisation/runner_roles.py
"""
import os
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from diarisation.runner import cluster_votes, consults, gold, run  # noqa: E402

GROUPING = 0.68


def parse(out):
    doctor, margin, vps, slices = -1, 0.0, {}, []
    for line in out.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "DOCTOR":
            doctor = int(parts[1])
        elif parts[0] == "MARGIN":
            margin = float(parts[1])
        elif parts[0] == "VP":
            vps[int(parts[1])] = np.array([float(x) for x in parts[2:]], dtype=np.float64)
        elif parts[0] == "SLICE":
            slices.append((float(parts[1]), float(parts[2]), int(parts[3])))
    return doctor, margin, vps, slices


def main():
    runs = {}
    tally = {"correct": 0, "abstain": 0, "WRONG": 0}
    for consult in consults():
        doctor, margin, vps, slices = parse(run(consult, "--roles"))
        votes = cluster_votes(slices, gold(consult))
        truth = max(votes, key=lambda c: votes[c][1] - votes[c][0])
        runs[consult] = {"truth": truth, "vps": vps, "slices": slices}
        verdict = "abstain" if doctor < 0 else "correct" if doctor == truth else "WRONG"
        tally[verdict] += 1
        print(f"{consult}: cold-start {verdict} (margin {margin:.2f})", flush=True)
    print(f"\nCOLD START: {tally['correct']} correct, {tally['abstain']} abstained, "
          f"{tally['WRONG']} wrong of {len(runs)}")

    prints = {c: r["vps"][r["truth"]] for c, r in runs.items() if r["truth"] in r["vps"]}
    names = sorted(prints)
    parent = {c: c for c in names}

    def root(c):
        while parent[c] != c:
            c = parent[c]
        return c

    for i, a in enumerate(names):
        for b in names[i + 1:]:
            if float(np.dot(prints[a], prints[b])) > GROUPING:
                parent[root(a)] = root(b)
    groups = defaultdict(list)
    for c in names:
        groups[root(c)].append(c)
    recurring = [g for g in groups.values() if len(g) > 1]
    print(f"clinicians found: {len(groups)} ({len(recurring)} recurring, "
          f"covering {sum(map(len, recurring))} consults)")

    right = wrong = 0
    for group in recurring:
        for held_out in group:
            anchor = np.mean([prints[c] for c in group if c != held_out], axis=0)
            anchor /= np.linalg.norm(anchor) + 1e-9
            r = runs[held_out]
            talk = defaultdict(float)
            for s, e, cl in r["slices"]:
                talk[cl] += e - s
            pair = sorted(talk, key=talk.get, reverse=True)[:2]
            sims = {cl: float(np.dot(anchor, r["vps"][cl])) for cl in pair if cl in r["vps"]}
            if len(sims) < 2:
                continue
            if max(sims, key=sims.get) == r["truth"]:
                right += 1
            else:
                wrong += 1
                print(f"  ANCHOR WRONG on {held_out}")
    print(f"ANCHOR leave-one-out: {right}/{right + wrong} correct")
    ok = tally["WRONG"] == 0 and wrong == 0
    print(f"[{'PASS' if ok else 'FAIL'}] zero wrong names on either path")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
