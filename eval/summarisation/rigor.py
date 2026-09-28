"""How far the judge can be trusted, from the records in runs/ and reps/.

  reliability  test-retest: the same notes judged in independent passes (reps/pass<k>/<tag>/),
               within-note spread of faithfulness and completeness, and checklist item agreement
  validity     judge faithfulness against the clinicians' own correct/incorrect labels on the
               PriMock candidate notes (prep/human_labels.json): Pearson, Spearman, error, bias,
               and whether the judge ranks each consultation's two notes the same way
  severity     omission criticality recalibrated: PriMock marks 85% of checklist items critical,
               so an omission counts as critical only when the item matches the safety taxonomy
               below (provisional, for clinician sign-off)

Each writes rigor/<name>.json under build/eval/summarisation.

    python eval/summarisation/rigor.py reliability
"""
import argparse
import collections
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import read_json, write_json  # noqa: E402
from score import records, score_note  # noqa: E402

# Clinically material if missed
CRITICAL_PATTERNS = [
    r"allerg|nkda",                                                                     # allergy status
    r"\bmedicat|\bdrug|\bmg\b|dose|inhaler|tablet|spray|prescrib|steroid|antibiotic",  # medicines and doses
    r"chest pain|breath|short of breath|\bsob\b|palpitation",                           # cardiorespiratory red flags
    r"blood|bleed|haematuria|melaena|haemoptysis",                                      # bleeding
    r"weakness|numbness|speech|vision|neuro|photophobia|neck stiff|seizure|collapse|faint|dizz",  # neurological
    r"weight loss|fever|night sweat|lump|mass",                                         # systemic red flags
    r"suicid|self harm|self-harm|low mood|risk",                                        # mental health risk
    r"diagnos|refer|admit|safety|follow.?up|red flag|safeguard",                        # plan and safety-netting
    r"pregnan|lmp|sexual",                                                              # pregnancy, sexual health
]
CRITICAL = re.compile("|".join(CRITICAL_PATTERNS), re.I)


def recalibrate(item_text):
    return "critical" if CRITICAL.search(item_text) else "minor"


def out(name, obj):
    write_json(config.out("summarisation", "rigor") / f"{name}.json", obj)


def reliability():
    data = config.path("summarisation")
    passes = sorted((data / "reps").glob("pass*"))
    if len(passes) < 2:
        raise SystemExit("reliability needs two or more passes under reps/")
    faith_sds, comp_sds, agreement, per_note = [], [], [], []
    for tag in config.section("summarisation")["reliability"]:
        for cid in sorted(p.stem for p in (passes[0] / tag).glob("*.json")):
            paths = [p / tag / f"{cid}.json" for p in passes]
            if not all(p.exists() for p in paths):
                continue
            judged = [read_json(p) for p in paths]
            scored = [score_note(j) for j in judged]
            f = [s["faithfulness"] for s in scored if s["faithfulness"] is not None]
            c = [s["completeness"] for s in scored if s["completeness"] is not None]
            fsd = statistics.pstdev(f) if len(f) > 1 else 0.0
            csd = statistics.pstdev(c) if len(c) > 1 else 0.0
            faith_sds.append(fsd)
            comp_sds.append(csd)
            per_note.append((tag, cid, min(f), max(f), fsd, min(c), max(c), csd))
            votes = collections.defaultdict(list)
            for j in judged:
                for item in j.get("checklist", []):
                    votes[item["id"]].append(bool(item.get("present")))
            for v in votes.values():
                if len(v) == len(judged):
                    majority = sum(v) >= len(v) / 2
                    agreement.append(sum(1 for x in v if x == majority) / len(v))
    print(f"reliability over {len(passes)} passes, {len(per_note)} notes")
    print(f"  faithfulness within-note SD: mean {statistics.mean(faith_sds):.3f}  max {max(faith_sds):.3f}")
    print(f"  completeness within-note SD: mean {statistics.mean(comp_sds):.3f}  max {max(comp_sds):.3f}")
    print(f"  checklist present/absent agreement: {statistics.mean(agreement) * 100:.1f}% (n={len(agreement)})")
    for tag, cid, fmn, fmx, fsd, cmn, cmx, csd in per_note:
        print(f"  {tag:32} {cid:22} faith {fmn:.2f}-{fmx:.2f} sd{fsd:.2f}   compl {cmn:.2f}-{cmx:.2f} sd{csd:.2f}")
    out("reliability", {"n_passes": len(passes), "n_notes": len(per_note),
                        "faith_sd_mean": round(statistics.mean(faith_sds), 4), "faith_sd_max": round(max(faith_sds), 4),
                        "comp_sd_mean": round(statistics.mean(comp_sds), 4), "comp_sd_max": round(max(comp_sds), 4),
                        "item_agreement": round(statistics.mean(agreement), 4)})


def pearson(xs, ys):
    mx, my = statistics.mean(xs), statistics.mean(ys)
    cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sx = sum((x - mx) ** 2 for x in xs) ** 0.5
    sy = sum((y - my) ** 2 for y in ys) ** 0.5
    return cov / (sx * sy) if sx and sy else float("nan")


def spearman(xs, ys):
    def rank(v):
        r = [0] * len(v)
        for pos, i in enumerate(sorted(range(len(v)), key=lambda i: v[i])):
            r[i] = pos
        return r
    return pearson(rank(xs), rank(ys))


def validity():
    human = read_json(config.path("summarisation") / "prep" / "human_labels.json")
    judged = {n: records(f"human-c1-{n}") for n in ("note1", "note2")}
    hu, ju, rows = [], [], []
    agree = total = 0
    for cid, rec in sorted(human.items()):
        pair = {}
        for n in ("note1", "note2"):
            if n not in rec or cid not in judged[n]:
                continue
            jf = score_note(judged[n][cid])["faithfulness"]
            hf = rec[n]["human_faith"]
            hu.append(hf)
            ju.append(jf)
            pair[n] = (hf, jf)
            rows.append((cid, n, hf, jf))
        if len(pair) == 2:
            total += 1
            (h1, j1), (h2, j2) = pair["note1"], pair["note2"]
            if h1 == h2 or j1 == j2:
                agree += 0.5
            elif (h1 - h2) * (j1 - j2) > 0:
                agree += 1
    result = {"n_pairs": len(hu), "pearson": round(pearson(hu, ju), 3), "spearman": round(spearman(hu, ju), 3),
              "mae": round(statistics.mean(abs(j - h) for h, j in zip(hu, ju)), 3),
              "bias": round(statistics.mean(j - h for h, j in zip(hu, ju)), 3), "rank_agree": f"{agree}/{total}"}
    for cid, n, hf, jf in rows:
        print(f"  {cid:22} {n:6} human {hf:.3f} judge {jf:.3f} ({jf - hf:+.3f})")
    print(result)
    out("validity", result)


def severity():
    checklists = sorted((config.path("summarisation") / "prep" / "checklists").glob("*.json"))
    inherited, recal = collections.Counter(), collections.Counter()
    for path in checklists:
        for item in read_json(path):
            inherited[item["criticality"]] += 1
            recal[recalibrate(item["text"])] += 1
    total = sum(inherited.values())
    print(f"inherited (PriMock c/nc): critical {inherited['critical']} ({100 * inherited['critical'] / total:.0f}%)")
    print(f"recalibrated (taxonomy):  critical {recal['critical']} ({100 * recal['critical'] / total:.0f}%)")
    print(f"critical omissions per note, inherited / recalibrated:")
    for tag in config.section("summarisation")["bootstrap"]:
        recs = records(tag).values()
        per_note = [[it for it in j.get("checklist", []) if not it.get("present")] for j in recs]
        old = [sum(1 for it in n if it.get("severity") == "critical") for n in per_note]
        new = [sum(1 for it in n if recalibrate(it["item"]) == "critical") for n in per_note]
        if per_note:
            print(f"  {tag:32} {statistics.mean(old):>5.1f} {statistics.mean(new):>5.1f}")
    out("severity", {"inherited_pct_critical": round(100 * inherited["critical"] / total),
                     "recalibrated_pct_critical": round(100 * recal["critical"] / total)})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("check", choices=["reliability", "validity", "severity"])
    args = ap.parse_args()
    {"reliability": reliability, "validity": validity, "severity": severity}[args.check]()


if __name__ == "__main__":
    main()
