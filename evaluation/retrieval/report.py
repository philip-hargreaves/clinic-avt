"""Tables from evaluation runs, written to rag/results/report-*.md.

  python report.py embedders [--ref bge-base] [--mode sentence]   first stage per embedder from its latest complete run, with paired wins and a bootstrap CI on s@10
  python report.py rerankers                                      each reranker against none on the same 50 candidates, per embedder
  python report.py ordering <run> [--mode sentence]               ordering rules on a --dump-union run: rerank 50/20/10, RRF, abstention coverage
  python report.py second-stage <run> [--mode both] [--pool a,b]  every second-stage rule against cosine order: s@3 endpoint, bootstrap CI, sign test
  python report.py union-rules <run>... [--mode both]             runs differing only in --union, paired by query
  python report.py sources <run>... [--mode both] [--sets ...]    the same consultations queried from different texts, paired by consultation

Paired statistics are on identical query sets. RRF is reciprocal rank fusion (Cormack, Clarke
and Buettcher 2009, k=60). nDCG follows Jarvelin and Kekalainen 2002.
"""

import argparse
import csv
import json
import math
import os
import random
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import read_json, read_jsonl  # noqa: E402
from harness import RESULTS, load_shortlist  # noqa: E402

FULL_QUERY_FILE = 500  # smoke runs use fewer queries
K_RRF = 60
SETS = ["primock", "synthetic", "st-georges", "ucl"]
LABELS = {"r10": "s@10", "r50": "s@50", "rec50": "rec@50", "p1": "p@1", "mrr": "MRR", "ndcg10": "nDCG@10", "entity_top1": "entity"}


def complete_runs(embedder: str, precision="int8"):
    """Oldest first: (dir, config, rows) for finished plain-text runs over the full query file."""
    for run in sorted(RESULTS.glob(f"*-eval-{embedder}")):
        if not (run / "per_query.jsonl").exists() or not (run / "config.json").exists():
            continue
        try:
            config = read_json(run / "config.json")
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue  # truncated by a killed session
        if config.get("precision") != precision or config.get("field") != "text":
            continue
        if config.get("queries", 0) < FULL_QUERY_FILE:
            continue
        yield run, config, read_jsonl(run / "per_query.jsonl")


def shortlisted(*roles):
    return [e for e in load_shortlist() if e["role"] in roles]


def mean(rows, key):
    vals = [r[key] for r in rows if r.get(key) is not None]
    return sum(vals) / len(vals) if vals else None


def cell(x):
    return "" if x is None else f"{x:.2f}"


def bootstrap(values, n=2000, seed=0):
    rnd = random.Random(seed)
    means = sorted(sum(rnd.choice(values) for _ in values) / len(values) for _ in range(n))
    return means[int(0.025 * n)], means[int(0.975 * n)]


def write(dest: Path, lines):
    dest.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print("\n".join(lines))
    print(f"-> {dest}")


# embedders

def first_stage(rows, mode):
    if mode not in {r["mode"] for r in rows}:
        mode = "note"  # the v1 baseline exists in note mode only
    return {r["qid"]: r for r in rows if r["mode"] == mode and r["hybrid"] == "off" and r["reranker"] == "none" and not r["negative"]}


def embedders(args):
    runs, data = {}, {}
    for e in shortlisted("embedder", "baseline"):
        # the v1 baseline towers were staged and evaluated at fp16 only
        for run, _, rows in complete_runs(e["id"], "fp16" if e["role"] == "baseline" else "int8"):
            stage = first_stage(rows, args.mode)
            if stage:
                runs[e["id"]], data[e["id"]] = run, stage
    metrics = list(LABELS)
    out = [f"# Embedders, first stage only, {args.mode} mode", "",
           "Latest complete evaluation per embedder. s@k: a labelled recommendation in the top k. rec@50: share of "
           f"labels in the top 50. Wins/losses: queries where the embedder beats/trails {args.ref} on s@10. "
           "Interval: bootstrap 95% over queries for s@10.", ""]
    for s in SETS:
        out += [f"## {s}", "", "| embedder | n | " + " | ".join(LABELS[m] for m in metrics) + f" | wins/losses vs {args.ref} | s@10 95% CI |",
                "|---|---|" + "---|" * len(metrics) + "---|---|"]
        ref = {q: r for q, r in data.get(args.ref, {}).items() if r["set"] == s}
        for e, rows in data.items():
            rs = [r for r in rows.values() if r["set"] == s]
            if not rs:
                continue
            wins = sum(1 for r in rs if r["qid"] in ref and r["r10"] > ref[r["qid"]]["r10"])
            losses = sum(1 for r in rs if r["qid"] in ref and r["r10"] < ref[r["qid"]]["r10"])
            lo, hi = bootstrap([r["r10"] for r in rs]) if len(rs) > 2 else (0, 0)
            out.append(f"| {e} | {len(rs)} | " + " | ".join(cell(mean(rs, m)) for m in metrics) + f" | {wins}/{losses} | {lo:.2f} to {hi:.2f} |")
        out.append("")
    out += ["Runs used:", ""] + [f"- {e}: `{r.name}`" for e, r in runs.items()] + [""]
    write(RESULTS / "report-embedders.md", out)


# rerankers

def rerankers(args):
    metrics = ["r10", "p1", "mrr", "ndcg10", "entity_top1"]
    out = ["# Rerankers on the leading embedders, INT8, sentence mode", "",
           "Each row reranks the same 50 first-stage candidates as the 'none' row above it. Wins/losses: queries where the "
           "reranker's p@1 beats/trails no reranker. Time: seconds per query for the whole set, reranking included.", ""]
    for e in [x["id"] for x in shortlisted("embedder")]:
        found = {}  # reranker -> (run, rows), latest run containing it
        for run, config, rows in complete_runs(e):
            if "off" not in config.get("hybrid", "off").split(","):
                continue
            for name in {r["reranker"] for r in rows} - {"none"}:
                found[name] = (run, rows)
        if not found:
            continue
        for s in ["primock", "synthetic", "ucl"]:
            out += [f"## {e}, {s}", "", "| reranker | n | " + " | ".join(LABELS[m] for m in metrics) + " | wins/losses on p@1 | s per query |",
                    "|---|---|" + "---|" * len(metrics) + "---|---|"]
            base_written = False
            for name, (run, rows) in sorted(found.items()):
                pos = [r for r in rows if r["set"] == s and r["mode"] == "sentence" and r["hybrid"] == "off" and not r["negative"]]
                none = {r["qid"]: r for r in pos if r["reranker"] == "none"}
                rr = [r for r in pos if r["reranker"] == name]
                if not rr:
                    continue
                if not base_written:
                    n0 = list(none.values())
                    out.append(f"| none | {len(n0)} | " + " | ".join(cell(mean(n0, m)) for m in metrics) + " | reference | |")
                    base_written = True
                wins = sum(1 for r in rr if r["qid"] in none and r["p1"] > none[r["qid"]]["p1"])
                losses = sum(1 for r in rr if r["qid"] in none and r["p1"] < none[r["qid"]]["p1"])
                per_q = ""
                if (run / "metrics.csv").exists():
                    total = sum(1 for r in rows if r["reranker"] == name and r["mode"] == "sentence")
                    for row in csv.DictReader(open(run / "metrics.csv", encoding="utf-8")):
                        if row.get("set") == s and row.get("reranker") == name and row.get("mode") == "sentence":
                            per_q = f"{float(row['seconds']) / max(total, 1):.2f}"
                out.append(f"| {name} | {len(rr)} | " + " | ".join(cell(mean(rr, m)) for m in metrics) + f" | {wins}/{losses} | {per_q} |")
            out.append("")
    write(RESULTS / "report-rerankers.md", out)


# ordering

def load_union(run: str, mode: str):
    """qid -> {set, negative, expected, cands: {id: {cos, <reranker>: score}}} from a --dump-union run."""
    out, names = {}, set()
    for r in read_jsonl(RESULTS / run / "per_query.jsonl"):
        if r["mode"] != mode or r["hybrid"] != "off" or not r.get("union"):
            continue
        q = out.setdefault(r["qid"], {"set": r["set"], "negative": r["negative"], "expected": set(r["expected_ids"]), "cands": {}})
        for c in r["union"]:
            entry = q["cands"].setdefault(c["id"], {"cos": c["cos"]})
            if r["reranker"] != "none":
                entry[r["reranker"]] = c["score"]
                names.add(r["reranker"])
    if not out:
        raise SystemExit(f"{run} has no union rows; rerun evaluate.py with --dump-union")
    return out, sorted(names)


def order_by(cands, key):
    return sorted([i for i in cands if key in cands[i]], key=lambda i: -cands[i][key])


def topk_rerank(cands, key, k):
    cos = order_by(cands, "cos")
    return sorted(cos[:k], key=lambda i: -cands[i].get(key, -1e9)) + cos[k:]


def rrf_scores(cands, key):
    cr = {i: r for r, i in enumerate(order_by(cands, "cos"))}
    rr = {i: r for r, i in enumerate(order_by(cands, key))}
    return {i: 1 / (K_RRF + cr[i]) + 1 / (K_RRF + rr.get(i, 10_000)) for i in cr}


def rrf_order(cands, key):
    scores = rrf_scores(cands, key)
    return sorted(scores, key=lambda i: -scores[i])


def rank_metrics(order, expected):
    hits = [1 if i in expected else 0 for i in order]
    first = next((k for k, h in enumerate(hits) if h), None)
    return {"s10": int(any(hits[:10])), "p1": hits[0] if hits else 0, "mrr": 0.0 if first is None else 1 / (first + 1)}


def coverage_at_fp5(pos_scores, neg_scores):
    best = None
    for t in sorted(set(pos_scores + neg_scores)):
        cov = sum(1 for s in pos_scores if s >= t) / len(pos_scores)
        fp = sum(1 for s in neg_scores if s >= t) / len(neg_scores)
        if fp <= 0.05 and (best is None or cov > best[0]):
            best = (cov, t)
    return best


def ordering(args):
    queries, names = load_union(args.run, args.mode)
    negatives = [q for q in queries.values() if q["negative"]]

    def top_score(q, key):
        if key.startswith("rrf:"):
            scores = rrf_scores(q["cands"], key.split(":", 1)[1])
            return max(scores.values()) if scores else None
        vals = [c[key] for c in q["cands"].values() if key in c]
        return max(vals) if vals else None

    out = [f"# Ordering rules and abstention: {args.run}, {args.mode} mode", "",
           f"Identical 50-candidate lists per query; only the ordering rule and the abstention score change. RRF k={K_RRF}. "
           f"Coverage: share of positives kept at the largest threshold holding false positives to 5% of the {len(negatives)} negatives.", ""]
    for s in args.sets.split(","):
        pos = [q for q in queries.values() if q["set"] == s and not q["negative"]]
        if not pos:
            continue
        out += [f"## {s} ({len(pos)} positives)", "", "| ordering | s@10 | p@1 | MRR | coverage at FP<=5% | abstention score |", "|---|---|---|---|---|---|"]
        variants = [("cosine only", lambda c: order_by(c, "cos"), "cos")]
        for rk in names:
            variants += [(f"{rk}, rerank 50", lambda c, rk=rk: order_by(c, rk) or order_by(c, "cos"), rk),
                         (f"{rk}, rerank top 20", lambda c, rk=rk: topk_rerank(c, rk, 20), rk),
                         (f"{rk}, rerank top 10", lambda c, rk=rk: topk_rerank(c, rk, 10), rk),
                         (f"{rk}, RRF with cosine", lambda c, rk=rk: rrf_order(c, rk), f"rrf:{rk}")]
        for name, order_fn, score_key in variants:
            ms = [rank_metrics(order_fn(q["cands"]), q["expected"]) for q in pos]
            ps = [x for x in (top_score(q, score_key) for q in pos) if x is not None]
            ns = [x for x in (top_score(q, score_key) for q in negatives) if x is not None]
            cov = coverage_at_fp5(ps, ns) if ps and ns else None
            cov_s = f"{cov[0]:.2f} (t={cov[1]:.3f})" if cov else "n/a"
            out.append(f"| {name} | {sum(m['s10'] for m in ms) / len(ms):.2f} | {sum(m['p1'] for m in ms) / len(ms):.2f} | "
                       f"{sum(m['mrr'] for m in ms) / len(ms):.2f} | {cov_s} | {score_key} |")
        out.append("")
    write(RESULTS / f"report-ordering-{args.run}.md", out)


# second stage: every rule against cosine order on identical candidate lists. The endpoint is
# s@3 and the decision is the bootstrap interval

K_SWEEP = (3, 5, 10, 20, 50)
ALPHAS = [i / 10 for i in range(11)]
BOOT = 5000


def guideline(cid):
    return cid.split("-", 1)[0]


def full_metrics(order, expected):
    hits = [1 if i in expected else 0 for i in order]
    first = next((k for k, h in enumerate(hits) if h), None)
    gl = {guideline(e) for e in expected}
    dcg = sum(h / math.log2(k + 2) for k, h in enumerate(hits[:3]))
    ideal = sum(1 / math.log2(k + 2) for k in range(min(3, len(expected))))
    return {"s3": int(any(hits[:3])), "ndcg3": dcg / ideal if ideal else 0.0, "p1": hits[0] if hits else 0,
            "mrr": 0.0 if first is None else 1 / (first + 1), "s10": int(any(hits[:10])),
            "g3": int(any(guideline(i) in gl for i in order[:3]))}


def minmax(vals):
    lo, hi = min(vals), max(vals)
    return [(v - lo) / (hi - lo) if hi > lo else 0.5 for v in vals]


def interpolate(cands, key, alpha):
    """Convex combination of min-max normalised cosine and reranker score within the candidate list."""
    ids = [i for i in cands if key in cands[i]]
    if not ids:
        return order_by(cands, "cos"), {}
    c, r = minmax([cands[i]["cos"] for i in ids]), minmax([cands[i][key] for i in ids])
    score = {i: alpha * c[k] + (1 - alpha) * r[k] for k, i in enumerate(ids)}
    return sorted(score, key=lambda i: -score[i]) + [i for i in order_by(cands, "cos") if i not in score], score


def sign_test(w, l):
    n = w + l
    if n == 0:
        return 1.0
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(min(w, l) + 1)) / 2 ** n)


def paired(base, new, key, seed=0):
    d = [n[key] - b[key] for b, n in zip(base, new)]
    n = len(d)
    rnd = random.Random(seed)
    means = sorted(sum(rnd.choice(d) for _ in range(n)) / n for _ in range(BOOT))
    w, l = sum(1 for x in d if x > 0), sum(1 for x in d if x < 0)
    return {"delta": sum(d) / n, "lo": means[int(0.025 * BOOT)], "hi": means[int(0.975 * BOOT)],
            "wins": w, "losses": l, "p": sign_test(w, l), "net100": 100 * (w - l) / n}


def aurc(pos_scores, neg_scores):
    """Area under the risk-coverage curve: risk = share of accepted queries that are negatives (lower is better)."""
    items = sorted([(s, 0) for s in pos_scores] + [(s, 1) for s in neg_scores], key=lambda x: -x[0])
    risks, neg = [], 0
    for k, (_, is_neg) in enumerate(items, 1):
        neg += is_neg
        risks.append(neg / k)
    return sum(risks) / len(risks) if risks else float("nan")


def coverage_at(pos_scores, neg_scores, fp_max):
    best = 0.0
    for t in sorted(set(pos_scores + neg_scores)):
        fp = sum(1 for s in neg_scores if s >= t) / len(neg_scores)
        if fp <= fp_max:
            best = max(best, sum(1 for s in pos_scores if s >= t) / len(pos_scores))
    return best


def second_stage(args):
    queries, names = load_union(args.run, args.mode)
    sets = [s for s in args.sets.split(",") if s]
    pool = [s for s in args.pool.split(",") if s]
    negatives = [q for q in queries.values() if q["negative"]]
    by_set = {s: [q for q in queries.values() if q["set"] == s and not q["negative"]] for s in sets}
    by_set = {s: v for s, v in by_set.items() if v}
    if not by_set:
        raise SystemExit(f"no positives in sets {sets} for run {args.run}, mode {args.mode}")

    def alpha_for(rk, target_set):
        """alpha chosen on the other sets, or in-sample if there is no other set. The report says which."""
        others = [q for s, qs in by_set.items() if s != target_set for q in qs]
        insample = target_set is None or not others
        train = by_set.get(target_set, []) if insample and target_set else (others if not insample else [q for qs in by_set.values() for q in qs])
        best = max(ALPHAS, key=lambda a: sum(full_metrics(interpolate(q["cands"], rk, a)[0], q["expected"])["s3"] for q in train))
        return best, insample

    def variants(target_set):
        v = [("cosine only", lambda c: order_by(c, "cos"), "cos")]
        for rk in names:
            for k in K_SWEEP:
                v.append((f"{rk} over top {k}", lambda c, rk=rk, k=k: topk_rerank(c, rk, k), rk if k == 50 else None))
            v.append((f"{rk} RRF with cosine", lambda c, rk=rk: rrf_order(c, rk), f"rrf:{rk}"))
            a, insample = alpha_for(rk, target_set)
            tag = "in-sample" if insample else "tuned on the other sets"
            v.append((f"{rk} interpolated, alpha {a:.1f} ({tag})", lambda c, rk=rk, a=a: interpolate(c, rk, a)[0], None))
        return v

    out = [f"# Second stage: {args.run}, {args.mode} mode", "",
           "Pre-registered endpoint: s@3 (a labelled recommendation among the three shown), paired against cosine "
           "order on identical 50-candidate lists. Decision rule: a second stage ships only if the pooled s@3 "
           f"interval excludes zero in its favour. Bootstrap {BOOT} resamples; sign test two-sided; net = wins minus "
           "losses per 100 queries. nDCG@3 (Jarvelin and Kekalainen 2002) beside it; g@3 = right guideline in the top 3.", ""]

    def table(title, qs, target_set):
        base_order = [order_by(q["cands"], "cos") for q in qs]
        base = [full_metrics(o, q["expected"]) for o, q in zip(base_order, qs)]
        out.extend([f"## {title} ({len(qs)} positives)", "",
                    "| ordering | s@3 | delta | 95% CI | wins/losses | sign p | net/100 | nDCG@3 | p@1 | MRR | s@10 | g@3 |",
                    "|---|---|---|---|---|---|---|---|---|---|---|---|"])
        rows = {}
        for name, fn, _ in variants(target_set):
            ms = [full_metrics(fn(q["cands"]), q["expected"]) for q in qs]
            mean = {k: sum(m[k] for m in ms) / len(ms) for k in ms[0]}
            if name == "cosine only":
                out.append(f"| {name} | {mean['s3']:.2f} | reference | | | | | {mean['ndcg3']:.2f} | {mean['p1']:.2f} | {mean['mrr']:.2f} | {mean['s10']:.2f} | {mean['g3']:.2f} |")
            else:
                st = paired(base, ms, "s3")
                out.append(f"| {name} | {mean['s3']:.2f} | {st['delta']:+.2f} | {st['lo']:+.2f} to {st['hi']:+.2f} | {st['wins']}/{st['losses']} | {st['p']:.2f} | {st['net100']:+.0f} | "
                           f"{mean['ndcg3']:.2f} | {mean['p1']:.2f} | {mean['mrr']:.2f} | {mean['s10']:.2f} | {mean['g3']:.2f} |")
            rows[name] = ms
        out.append("")
        return base, rows

    if len(pool) > 1:
        pooled = [q for s in pool for q in by_set.get(s, [])]
        table(f"Pooled primary sets {', '.join(pool)}", pooled, None)
    for s, qs in by_set.items():
        base, rows = table(s, qs, s)
        if s == args.errors:
            # where the shipped rules change the top three: sibling recommendation or another guideline
            for name in [n for n in rows if " over top 10" in n or " over top 3" in n]:
                wins, losses = [], []
                for q, b, m in zip(qs, base, rows[name]):
                    if m["s3"] > b["s3"]:
                        wins.append(q)
                    elif m["s3"] < b["s3"]:
                        rk = name.split(" over top ")[0]
                        k = int(name.split(" over top ")[1])
                        top3 = topk_rerank(q["cands"], rk, k)[:3]
                        gl = {guideline(e) for e in q["expected"]}
                        kind = "sibling recommendations" if all(guideline(i) in gl for i in top3) else "another guideline in the top 3"
                        losses.append((q, kind))
                out.extend([f"### {s}: {name}, changes to the top three", "",
                            f"- wins ({len(wins)}): " + ", ".join(sorted(qid for qid, q in queries.items() if q in wins)),
                            f"- losses ({len(losses)}): " + ", ".join(f"{qid} ({kind})" for (q, kind) in losses for qid, qq in queries.items() if qq is q), ""])

    # abstention: which top score separates positives from negatives
    if negatives:
        pos = [q for s in (pool or list(by_set)) for q in by_set.get(s, [])]
        out.extend([f"## Abstention scores ({len(pos)} positives, {len(negatives)} negatives)", "",
                    "| top score | coverage at FP<=5% | coverage at FP<=10% | AURC (lower is better) |", "|---|---|---|---|"])
        scorers = [("cosine", lambda q: max(c["cos"] for c in q["cands"].values()))]
        for rk in names:
            scorers.append((rk, lambda q, rk=rk: max(c.get(rk, -1e9) for c in q["cands"].values())))
            scorers.append((f"{rk} interpolated, alpha 0.5", lambda q, rk=rk: max(interpolate(q["cands"], rk, 0.5)[1].values() or [0.0])))
        for name, fn in scorers:
            ps, ns = [fn(q) for q in pos], [fn(q) for q in negatives]
            out.append(f"| {name} | {coverage_at(ps, ns, 0.05):.2f} | {coverage_at(ps, ns, 0.10):.2f} | {aurc(ps, ns):.3f} |")
        out.append("")
    write(RESULTS / f"report-second-stage-{args.run}-{args.mode}.md", out)


# union rules: runs that differ only in how sub-query lists merge, paired

def top_metrics(top_ids, expected):
    hits = [1 if i in expected else 0 for i in top_ids]
    first = next((k for k, h in enumerate(hits) if h), None)
    gl = {guideline(e) for e in expected}
    return {"s3": int(any(hits[:3])), "s10": int(any(hits[:10])), "p1": hits[0] if hits else 0,
            "mrr10": 0.0 if first is None else 1 / (first + 1), "g3": int(any(guideline(i) in gl for i in top_ids[:3]))}


def union_rules(args):
    runs = args.runs
    rows, labels = {}, []
    for run in runs:
        label = read_json(RESULTS / run / "config.json").get("union", "max")
        if label not in labels:
            labels.append(label)
        for r in read_jsonl(RESULTS / run / "per_query.jsonl"):
            if r["mode"] == args.mode and r["hybrid"] == "off" and r["reranker"] == "none" and not r["negative"]:
                rows.setdefault(r["set"], {}).setdefault(r["qid"], {})[label] = top_metrics([i for i, _ in r["top"]], set(r["expected_ids"]))
    if args.mode == "both":
        # the whole-note query alone, from the first run's note-mode pass, as a reference row
        for r in read_jsonl(RESULTS / runs[0] / "per_query.jsonl"):
            if r["mode"] == "note" and r["hybrid"] == "off" and r["reranker"] == "none" and not r["negative"] and r["set"] in rows:
                rows[r["set"]].setdefault(r["qid"], {})["whole note only"] = top_metrics([i for i, _ in r["top"]], set(r["expected_ids"]))
        if any("whole note only" in q for s in rows.values() for q in s.values()):
            labels.append("whole note only")
    out = [f"# Union rules, {args.mode} mode: {', '.join(runs)}", "",
           f"Same embedder, same queries, first stage only; the runs differ in how the sub-query candidate lists merge. Paired against `{labels[0]}`; bootstrap {BOOT} resamples.", ""]
    for s in args.sets.split(","):
        qs = rows.get(s)
        if not qs:
            continue
        complete = [q for q in qs.values() if all(l in q for l in labels)]
        out.extend([f"## {s} ({len(complete)} positives)", "", "| union rule | s@3 | delta | 95% CI | wins/losses | s@10 | delta | p@1 | MRR@10 | g@3 |", "|---|---|---|---|---|---|---|---|---|---|"])
        base = [q[labels[0]] for q in complete]
        for l in labels:
            ms = [q[l] for q in complete]
            mean = {k: sum(m[k] for m in ms) / len(ms) for k in ms[0]}
            if l == labels[0]:
                out.append(f"| {l} | {mean['s3']:.2f} | reference | | | {mean['s10']:.2f} | | {mean['p1']:.2f} | {mean['mrr10']:.2f} | {mean['g3']:.2f} |")
            else:
                st3, st10 = paired(base, ms, "s3"), paired(base, ms, "s10")
                out.append(f"| {l} | {mean['s3']:.2f} | {st3['delta']:+.2f} | {st3['lo']:+.2f} to {st3['hi']:+.2f} | {st3['wins']}/{st3['losses']} | {mean['s10']:.2f} | {st10['delta']:+.2f} ({st10['lo']:+.2f} to {st10['hi']:+.2f}) | {mean['p1']:.2f} | {mean['mrr10']:.2f} | {mean['g3']:.2f} |")
        out.append("")
    write(RESULTS / f"report-union-rules-{args.mode}.md", out)


# sources: the same consultations queried from different texts (note, transcript, both), paired by consultation

def sources(args):
    sets = args.sets.split(",")
    rows = {}
    for run in args.runs:
        for r in read_jsonl(RESULTS / run / "per_query.jsonl"):
            if r["mode"] != args.mode or r["hybrid"] != "off" or r["reranker"] != "none" or r["negative"] or r["set"] not in sets:
                continue
            consult = r["qid"][len(r["set"]) + 1:]
            rows.setdefault(consult, {})[r["set"]] = top_metrics([i for i, _ in r["top"]], set(r["expected_ids"]))
    complete = [c for c in rows.values() if all(s in c for s in sets)]
    if not complete:
        raise SystemExit("no consultation has every set; check --runs and --sets")
    out = [f"# Query sources, {args.mode} mode ({args.label}): {', '.join(args.runs)}", "",
           f"The same {len(complete)} labelled consultations, queried from different texts. Paired against `{sets[0]}`; bootstrap {BOOT} resamples.", "",
           "| source | s@3 | delta | 95% CI | wins/losses | s@10 | delta (95% CI) | p@1 | MRR@10 | g@3 |", "|---|---|---|---|---|---|---|---|---|---|"]
    base = [c[sets[0]] for c in complete]
    for s in sets:
        ms = [c[s] for c in complete]
        mean = {k: sum(m[k] for m in ms) / len(ms) for k in ms[0]}
        if s == sets[0]:
            out.append(f"| {s} | {mean['s3']:.2f} | reference | | | {mean['s10']:.2f} | | {mean['p1']:.2f} | {mean['mrr10']:.2f} | {mean['g3']:.2f} |")
        else:
            st3, st10 = paired(base, ms, "s3"), paired(base, ms, "s10")
            out.append(f"| {s} | {mean['s3']:.2f} | {st3['delta']:+.2f} | {st3['lo']:+.2f} to {st3['hi']:+.2f} | {st3['wins']}/{st3['losses']} | {mean['s10']:.2f} | {st10['delta']:+.2f} ({st10['lo']:+.2f} to {st10['hi']:+.2f}) | {mean['p1']:.2f} | {mean['mrr10']:.2f} | {mean['g3']:.2f} |")
    out.append("")
    write(RESULTS / f"report-sources-{args.mode}-{args.label}.md", out)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("sources")
    p.add_argument("runs", nargs="+")
    p.add_argument("--mode", default="both")
    p.add_argument("--sets", default="notes-human,note-plus-doctor,transcript-doctor,transcript-full")
    p.add_argument("--label", default="max")
    p.set_defaults(fn=sources)
    p = sub.add_parser("union-rules")
    p.add_argument("runs", nargs="+")
    p.add_argument("--mode", default="both")
    p.add_argument("--sets", default="notes-human,notes-4b,notes-9b,notes-35b,primock,synthetic,st-georges")
    p.set_defaults(fn=union_rules)
    p = sub.add_parser("embedders")
    p.add_argument("--ref", default="bge-base")
    p.add_argument("--mode", default="sentence")
    p.set_defaults(fn=embedders)
    p = sub.add_parser("rerankers")
    p.set_defaults(fn=rerankers)
    p = sub.add_parser("ordering")
    p.add_argument("run")
    p.add_argument("--sets", default="primock,synthetic")
    p.add_argument("--mode", default="sentence")
    p.set_defaults(fn=ordering)
    p = sub.add_parser("second-stage")
    p.add_argument("run")
    p.add_argument("--mode", default="sentence")
    p.add_argument("--sets", default="primock,synthetic,ucl", help="sets reported, each on its own")
    p.add_argument("--pool", default="primock,synthetic", help="sets pooled for the primary endpoint; empty for none")
    p.add_argument("--errors", default="primock", help="set whose top-three changes are listed")
    p.set_defaults(fn=second_stage)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
