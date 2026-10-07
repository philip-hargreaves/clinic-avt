"""Scores from the judge records in runs/<tag>/. Deterministic, no model calls.

  notes <tag>...   per note and median: faithfulness = 1 - fabrications/claims (all judged
                   consults), completeness = present/items (the 20 with a checklist),
                   critical and minor fabrications and omissions per note -> scores/<tag>.json
  screening        one row per screening model: knowledge, speed, faithfulness and completeness
                   with 95% bootstrap intervals, critical fabrications and recalibrated critical
                   omissions -> scores/screening_table.{md,json}
  bootstrap        paired bootstrap of the medians over the consults every model has, with
                   pairwise differences, for the [summarisation] bootstrap models

Severity is reported as counts for comparison, with no pass/fail gate. Intervals are
percentile bootstraps over consults, 5000 resamples, seed 12345.

    python evaluation/summarisation/score.py notes tier-default-concise
    python evaluation/summarisation/score.py screening
"""
import argparse
import json
import os
import random
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import read_json, write_json  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
B, SEED = 5000, 12345


def score_note(j):
    claims = j.get("claims", [])
    fabs = [c for c in claims if c.get("verdict") != "supported"]
    checklist = j.get("checklist", [])
    present = [c for c in checklist if c.get("present")]
    absent = [c for c in checklist if not c.get("present")]
    return {
        "claims": len(claims), "faithfulness": (len(claims) - len(fabs)) / len(claims) if claims else None,
        "fabrications": len(fabs),
        "fab_critical": sum(1 for c in fabs if c.get("severity") == "critical"),
        "fab_minor": sum(1 for c in fabs if c.get("severity") == "minor"),
        "checklist_items": len(checklist), "present": len(present),
        "completeness": len(present) / len(checklist) if checklist else None,
        "omissions": len(absent),
        "omis_critical": sum(1 for c in absent if c.get("severity") == "critical"),
        "omis_minor": sum(1 for c in absent if c.get("severity") == "minor"),
    }


def records(tag):
    folder = config.path("summarisation") / "runs" / tag
    return {p.stem: read_json(p) for p in sorted(folder.glob("*.json"))}


def scores_dir():
    return config.out("summarisation", "scores")


def score_tag(tag):
    rows = {}
    for cid, j in records(tag).items():
        rows[cid] = {**score_note(j), "note": j.get("note")}

    def med(key):
        vals = [r[key] for r in rows.values() if r[key] is not None]
        return round(statistics.median(vals), 3) if vals else None

    def mean(key):
        vals = [r[key] for r in rows.values() if r.get(key) is not None]
        return round(sum(vals) / len(vals), 2) if vals else None

    agg = {"n_notes": len(rows), "faithfulness_median": med("faithfulness"),
           "completeness_median": med("completeness"),
           "fab_critical_per_note": mean("fab_critical"), "fab_minor_per_note": mean("fab_minor"),
           "omis_critical_per_note": mean("omis_critical"), "omis_minor_per_note": mean("omis_minor")}
    write_json(scores_dir() / f"{tag}.json", {"tag": tag, "aggregate": agg, "per_consult": rows})
    print(f"\n{tag}\n{'consult':22} {'faith':>6} {'compl':>6}   {'fab(c/m)':>10}   {'omis(c/m)':>12}")
    for cid, r in rows.items():
        f = f'{r["faithfulness"]:.2f}' if r["faithfulness"] is not None else "  -"
        c = f'{r["completeness"]:.2f}' if r["completeness"] is not None else "  -"
        fab = f'{r["fabrications"]} ({r["fab_critical"]}/{r["fab_minor"]})'
        omis = f'{r["omissions"]} ({r["omis_critical"]}/{r["omis_minor"]})' if r["checklist_items"] else "-"
        print(f"{cid:22} {f:>6} {c:>6}   {fab:>10}   {omis:>12}")
    print(f"aggregate: {json.dumps(agg)}")


def boot_median_ci(vals, rng):
    if len(vals) < 2:
        return None, None
    n = len(vals)
    meds = sorted(statistics.median([vals[rng.randrange(n)] for _ in range(n)]) for _ in range(B))
    return round(meds[int(0.025 * B)], 3), round(meds[int(0.975 * B)], 3)


def screening():
    from rigor import recalibrate
    context = read_json(os.path.join(HERE, "screening-context.json"))
    rng = random.Random(SEED)
    rows = []
    for tag in config.section("summarisation")["screening"]:
        recs = records(tag)
        if not recs:
            continue
        scored = [score_note(j) for j in recs.values()]
        faith = [s["faithfulness"] for s in scored if s["faithfulness"] is not None]
        comp = [s["completeness"] for s in scored if s["completeness"] is not None]
        crit_fabs = [s["fab_critical"] for s in scored]
        crit_omis = [sum(1 for it in j["checklist"] if not it.get("present") and recalibrate(it["item"]) == "critical")
                     for j in recs.values() if j.get("checklist")]
        c = context.get(tag, {})
        rows.append({
            "model": config.label(tag), "tag": tag, "n_faith": len(faith), "n_comp": len(comp),
            "knowledge": c.get("knowledge"), "tok_s": c.get("tok_s"), "time_note_s": c.get("time_note_s"),
            "faith_median": round(statistics.median(faith), 3) if faith else None,
            "faith_ci": list(boot_median_ci(faith, rng)),
            "comp_median": round(statistics.median(comp), 3) if comp else None,
            "comp_ci": list(boot_median_ci(comp, rng)),
            "crit_fabs_per_note": round(statistics.mean(crit_fabs), 2) if crit_fabs else None,
            "crit_omis_per_note_recal": round(statistics.mean(crit_omis), 1) if crit_omis else None})

    def cell(v, d=2):
        return "-" if v is None else (f"{v:.{d}f}" if isinstance(v, float) else str(v))

    def ci(lo, hi):
        return "-" if lo is None else f"[{lo:.3f}, {hi:.3f}]"

    lines = ["| model | know | tok/s | t/note | faithfulness [95% CI] | completeness [95% CI] "
             "| crit fabs/note | crit omis/note* |", "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for r in rows:
        tn = "-" if r["time_note_s"] is None else f"{r['time_note_s']:.0f}s"
        lines.append(f"| {r['model']} | {cell(r['knowledge'], 1)} | {cell(r['tok_s'], 1)} | {tn} "
                     f"| {cell(r['faith_median'], 3)} {ci(*r['faith_ci'])} "
                     f"| {cell(r['comp_median'], 3)} {ci(*r['comp_ci'])} "
                     f"| {cell(r['crit_fabs_per_note'])} | {cell(r['crit_omis_per_note_recal'], 1)} |")
    note = ("\n\n*crit omis/note uses the provisional safety taxonomy (descriptive only, not a gate). "
            "n(faith)/n(comp) per model: " + ", ".join(f"{r['model']} {r['n_faith']}/{r['n_comp']}" for r in rows) + ".")
    print("\n".join(lines) + note)
    (scores_dir() / "screening_table.md").write_text("\n".join(lines) + note + "\n", encoding="utf-8")
    write_json(scores_dir() / "screening_table.json", rows)


def bootstrap():
    models = config.section("summarisation")["bootstrap"]
    rng = random.Random(SEED)

    def ci(vals):
        vals = sorted(vals)
        return round(vals[int(0.025 * len(vals))], 3), round(vals[int(0.975 * len(vals))], 3)

    for metric in ("faithfulness", "completeness"):
        data = {m: {cid: s[metric] for cid, j in records(m).items() if (s := score_note(j))[metric] is not None}
                for m in models}
        cids = sorted(set.intersection(*[set(d) for d in data.values()]))
        n = len(cids)
        pairs = [(a, b) for i, a in enumerate(models) for b in models[i + 1:]]
        boot = {m: [] for m in models}
        diffs = {p: [] for p in pairs}
        for _ in range(B):
            sample = [cids[rng.randrange(n)] for _ in range(n)]
            meds = {m: statistics.median([data[m][c] for c in sample]) for m in models}
            for m in models:
                boot[m].append(meds[m])
            for a, b in pairs:
                diffs[(a, b)].append(meds[a] - meds[b])
        print(f"\n=== {metric}  (n={n} consults) ===")
        for m in models:
            lo, hi = ci(boot[m])
            print(f"  {m:32} median {statistics.median([data[m][c] for c in cids]):.3f}  95% CI [{lo:.3f}, {hi:.3f}]")
        print("  pairwise differences (a - b), 95% CI:")
        for a, b in pairs:
            lo, hi = ci(diffs[(a, b)])
            print(f"    {a} - {b}: [{lo:+.3f}, {hi:+.3f}]  {'significant' if lo > 0 or hi < 0 else 'n.s. (CI spans 0)'}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    notes = sub.add_parser("notes")
    notes.add_argument("tags", nargs="+")
    sub.add_parser("screening")
    sub.add_parser("bootstrap")
    args = ap.parse_args()
    if args.cmd == "notes":
        for tag in args.tags:
            score_tag(tag)
    elif args.cmd == "screening":
        screening()
    else:
        bootstrap()


if __name__ == "__main__":
    main()
