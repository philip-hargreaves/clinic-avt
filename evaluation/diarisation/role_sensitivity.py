"""Sensitivity of clinician naming to its hand-set weights, on the PriMock runs of multispeaker.py.

A port of LexicalDoctorScore (engine/domain/core/diarisation/role_naming.cpp) re-scores the turns
the engine labelled doctor and patient in each --roles run, with each weight halved, kept or doubled
(81 settings) and with each cue removed. A consultation is named right when the doctor-labelled
turns average higher by at least the margin, not named inside the margin, and wrong otherwise. The
labels serve as truth only if the shipped weights name every consultation correctly. The first
output line reports this.

    python evaluation/diarisation/multispeaker.py run --real      (once, for the runs)
    python evaluation/diarisation/role_sensitivity.py
"""
import itertools
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config, primock  # noqa: E402

WEIGHTS = {"second": 7.0, "first": 11.0, "introduction": 8.0, "plan": 2.5}
MARGIN = 0.5  # kRoleMinMargin
FIRST = {"i", "my", "me", "i'm", "i've"}
SECOND = {"you", "your", "you're"}
INTRODUCTION = [p.split() for p in [
    "i'm dr", "i'm doctor", "i am dr", "i am doctor", "this is dr", "this is doctor",
    "my name is dr", "my name is doctor", "from gp at hand", "calling from"]]
PLAN = [p.split() for p in [
    "i'll", "i will", "i'd like", "i would", "i'm going to", "i want you to",
    "we'll", "we will", "we're going to", "we can", "let's"]]


def has(words, phrase):
    n = len(phrase)
    return any(words[i:i + n] == phrase for i in range(len(words) - n + 1))


def score(text, w):
    words = re.findall(r"[a-z']+", text.lower())
    if not words:
        return None
    first = sum(x in FIRST for x in words)
    second = sum(x in SECOND for x in words)
    bonus = 0.0
    if any(has(words, p) for p in INTRODUCTION):
        bonus += w["introduction"]
        first = 0
    plan = sum(has(words, p) for p in PLAN)
    if plan:
        first = max(0, first - plan)
        bonus += w["plan"] * min(plan, 3)
    return bonus + w["second"] * second / len(words) - w["first"] * first / len(words)


def turns():
    runs = config.out("diarisation", "multispeaker", "runs")
    out = []
    for consult in primock.consults():
        path = runs / f"{consult}.txt"
        if not path.exists():
            continue
        doctor, patient = [], []
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("TURN "):
                role, _, text = line[5:].partition("\t")
                {"doctor": doctor, "patient": patient}.get(role, []).append(text)
        out.append((doctor, patient))
    return out


def judge(data, w):
    right = unnamed = wrong = 0
    gaps = []
    for doctor, patient in data:
        means = []
        for texts in (doctor, patient):
            scores = [s for s in (score(t, w) for t in texts) if s is not None]
            means.append(sum(scores) / len(scores) if scores else 0.0)
        gap = means[0] - means[1]
        gaps.append(gap)
        if abs(gap) < MARGIN:
            unnamed += 1
        elif gap > 0:
            right += 1
        else:
            wrong += 1
    return right, unnamed, wrong, sorted(gaps)


def main():
    data = turns()
    if not data:
        raise SystemExit("no runs: python evaluation/diarisation/multispeaker.py run --real")
    right, unnamed, wrong, gaps = judge(data, WEIGHTS)
    print(f"{len(data)} consultations, shipped weights: right {right}, not named {unnamed}, "
          f"wrong {wrong}; doctor-patient gap min {gaps[0]:.2f}, median {gaps[len(gaps) // 2]:.2f}")
    if right != len(data):
        print("the shipped run did not name every consultation, so its labels are not truth")

    settings = []
    for factors in itertools.product([0.5, 1, 2], repeat=4):
        w = {k: v * f for (k, v), f in zip(WEIGHTS.items(), factors)}
        settings.append(judge(data, w)[:3])
    print(f"81 settings, each weight x0.5/x1/x2: all right in "
          f"{sum(r == len(data) for r, _, _ in settings)}, fewest right "
          f"{min(r for r, _, _ in settings)}, most not named {max(u for _, u, _ in settings)}, "
          f"most wrong {max(x for _, _, x in settings)}")

    for name, drop in [("no second-person", ["second"]), ("no first-person", ["first"]),
                       ("no introduction", ["introduction"]), ("no plan phrases", ["plan"]),
                       ("pronouns only", ["introduction", "plan"])]:
        w = {k: 0.0 if k in drop else v for k, v in WEIGHTS.items()}
        r, u, x, _ = judge(data, w)
        print(f"{name:17s} right {r}, not named {u}, wrong {x}")


if __name__ == "__main__":
    main()
