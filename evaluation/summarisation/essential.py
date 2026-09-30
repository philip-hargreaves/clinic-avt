"""Essential completeness: the share of a consultation's essential checklist items that a note covers.

Checklist items are categorised once by independent raters with `essential-rubric.md`, blind to every
note. The majority label of the primary raters decides each item; the check rater is a different
model and measures agreement only. Item presence comes from the judge records, so no note is re-judged.

    python evaluation/summarisation/essential.py agree
    python evaluation/summarisation/essential.py score <tag>... [--consults a,b,c]

Raters' labels live in <summarisation>/essential/rater<N>/<consult>.txt as `<number><TAB><code>`.
"""
import argparse
import collections
import json
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402

CATEGORIES = ["PC", "RF", "AP", "MA", "PH", "SH", "DT"]
DEFINITIONS = {
    "essential": {"PC", "RF", "AP", "MA"},
    "narrow": {"RF", "AP", "MA"},
    "broad": {"PC", "RF", "AP", "MA", "PH"},
}
PRIMARY_RATERS = ["rater1", "rater2", "rater3"]
CHECK_RATER = "rater4"


def root():
    return config.path("summarisation") / "essential"


def consults():
    return sorted(p.stem for p in (root() / "items").glob("*.txt"))


def labels(rater, cid):
    path = root() / rater / f"{cid}.txt"
    with open(path, encoding="utf-8") as f:
        return [line.split("\t")[1].strip() for line in f if "\t" in line]


def majority():
    out = {}
    for cid in consults():
        votes = [labels(r, cid) for r in PRIMARY_RATERS]
        if len({len(v) for v in votes}) != 1:
            raise SystemExit(f"{cid}: raters labelled different numbers of items")
        chosen = []
        for item in zip(*votes):
            top, count = collections.Counter(item).most_common(1)[0]
            # With no majority the rubric's tie rule applies: the essential category wins
            if count < 2:
                top = next((x for x in item if x in DEFINITIONS["essential"]), top)
            chosen.append(top)
        out[cid] = chosen
    return out


def fleiss(rows, cats):
    n = len(rows[0])
    totals = collections.Counter()
    agreement = []
    for row in rows:
        counts = collections.Counter(row)
        totals.update(counts)
        agreement.append((sum(v * v for v in counts.values()) - n) / (n * (n - 1)))
    p_bar = sum(agreement) / len(agreement)
    p_e = sum((totals[c] / (len(rows) * n)) ** 2 for c in cats)
    return (p_bar - p_e) / (1 - p_e)


def cohen(a, b, cats):
    observed = sum(x == y for x, y in zip(a, b)) / len(a)
    ca, cb = collections.Counter(a), collections.Counter(b)
    expected = sum(ca[c] * cb[c] for c in cats) / (len(a) ** 2)
    return (observed - expected) / (1 - expected)


def essential_flag(label):
    return "E" if label in DEFINITIONS["essential"] else "S"


def agree(_):
    rows, rows_es, check, maj_all = [], [], [], []
    chosen = majority()
    for cid in consults():
        votes = [labels(r, cid) for r in PRIMARY_RATERS]
        rows += [list(v) for v in zip(*votes)]
        rows_es += [[essential_flag(x) for x in v] for v in zip(*votes)]
        check += labels(CHECK_RATER, cid)
        maj_all += chosen[cid]
    counts = collections.Counter(maj_all)
    essential = sum(counts[c] for c in DEFINITIONS["essential"])
    result = {
        "items": len(maj_all), "consults": len(consults()),
        "fleiss_categories": round(fleiss(rows, CATEGORIES), 3),
        "fleiss_essential": round(fleiss(rows_es, ["E", "S"]), 3),
        "check_vs_majority_categories": round(cohen(check, maj_all, CATEGORIES), 3),
        "check_vs_majority_essential": round(cohen([essential_flag(x) for x in check],
                                                   [essential_flag(x) for x in maj_all], ["E", "S"]), 3),
        "by_category": {c: counts[c] for c in CATEGORIES}, "essential_items": essential,
    }
    with open(root() / "labels.json", "w", encoding="utf-8") as f:
        json.dump(chosen, f, indent=1)
    print(json.dumps(result, indent=1))


def coverage(tag, cid, chosen, cats):
    path = config.path("summarisation") / "runs" / tag / f"{cid}.json"
    if not path.exists():
        return None
    checklist = json.load(open(path, encoding="utf-8")).get("checklist") or []
    if len(checklist) != len(chosen[cid]):
        return None
    items = [c for c, label in zip(checklist, chosen[cid]) if cats is None or label in cats]
    return sum(bool(c.get("present")) for c in items) / len(items) if items else None


def score(args):
    chosen = majority()
    wanted = args.consults.split(",") if args.consults else consults()
    columns = list(DEFINITIONS) + ["secondary", "full"]
    secondary = set(CATEGORIES) - DEFINITIONS["essential"]
    print(f"{'tag':30}{'n':>4}" + "".join(f"{c:>11}" for c in columns))
    for tag in args.tags:
        values = {c: [] for c in columns}
        for cid in wanted:
            for col in columns:
                cats = {"secondary": secondary, "full": None}.get(col, DEFINITIONS.get(col))
                v = coverage(tag, cid, chosen, cats)
                if v is not None:
                    values[col].append(v)
        n = len(values["full"])
        if n:
            print(f"{tag:30}{n:>4}" + "".join(f"{st.mean(values[c]):>11.3f}" for c in columns))


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("agree").set_defaults(fn=agree)
    s = sub.add_parser("score")
    s.add_argument("tags", nargs="+")
    s.add_argument("--consults", help="comma-separated consultation ids; default all with labels")
    s.set_defaults(fn=score)
    args = parser.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
