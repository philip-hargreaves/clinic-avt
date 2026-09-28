"""Retrieval quality for one embedder run, with optional rerankers, lexical fusion and threshold rules.

  python evaluate.py bge-base --precision int8 [--field text] [--modes sentence,note,both]
                     [--rerankers gte-reranker-modernbert,minilm-l6] [--rerank-precision int8]
                     [--hybrid off,on] [--lexical-scope all|drug] [--topk 50] [--v1-baseline]
                     [--union max|rrf|zmax] [--note-weight 1] [--queries <file>] [--dump-union]
                     [--query-model medcpt-query] [--self-test 200]

Reads rag/results/emb/<id>-<precision>-<field>/docs.npy and the latest queries file (or --queries).
Writes rag/results/<stamp>-eval-<id>/{metrics.csv, thresholds.csv, per_query.jsonl, summary.md}
"""

import argparse
import csv
import math
import os
import re
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import log, read_json, read_jsonl, write_jsonl  # noqa: E402
from harness import DEVICE, RESULTS, candidate, candidate_dir, latest_chunks, run_dir  # noqa: E402
from embed import embed_queries, make_pipeline
from gold import split_sentences

DOSE = re.compile(r"\b\d+(\.\d+)?\s?(mg|mcg|micrograms?|g|ml|units?|iu|mmol)\b", re.I)
DRUG_SUFFIX = re.compile(r"\w+(pril|sartan|statin|olol|azole|mycin|cillin|formin|gliptin|flozin|parin|mab|nib|dipine|prazole|triptan|tidine|oxacin|cycline|vir|oxetine|apine|azepam|codone|profen|salazine|purinol|colchicine|febuxostat|methotrexate|prednisolone|insulin|warfarin|aspirin|paracetamol)\b", re.I)
QWEN3_PREFIX = "<|im_start|>system\nJudge whether the Document meets the requirements based on the Query and the Instruct provided. Note that the answer can only be \"yes\" or \"no\".<|im_end|>\n<|im_start|>user\n"
QWEN3_SUFFIX = "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"
QWEN3_TASK = "Given a sentence from a clinical note, judge whether the guideline recommendation applies to it"
METRIC_KEYS = ("r5", "r10", "r50", "rec10", "rec50", "p1", "mrr", "ndcg10")
THRESHOLD_RULES = ("score_max", "score_gap", "score_z")


def tokens(text: str) -> list[str]:
    return re.findall(r"[a-z0-9]+(?:\.[0-9]+)?", text.lower())


def lexical_query(text: str, scope: str) -> list[str]:
    if scope == "all":
        return tokens(text)
    keep = [m.group(0) for m in DOSE.finditer(text)] + [m.group(0) for m in DRUG_SUFFIX.finditer(text)]
    return tokens(" ".join(keep))


def rrf(rankings: list[list[int]], k: int = 60) -> dict[int, float]:
    fused = {}
    for ranking in rankings:
        for rank, idx in enumerate(ranking):
            fused[idx] = fused.get(idx, 0.0) + 1.0 / (k + rank + 1)
    return fused


def metrics(ranked_ids: list[str], expected: set[str]) -> dict:
    if not expected:
        raise ValueError("metrics need at least one expected id; a query expecting nothing is a negative")
    hits = [1 if cid in expected else 0 for cid in ranked_ids]
    first = next((i for i, h in enumerate(hits) if h), None)
    dcg = sum(h / math.log2(i + 2) for i, h in enumerate(hits[:10]))
    ideal = sum(1 / math.log2(i + 2) for i in range(min(len(expected), 10)))
    # s@k: at least one labelled recommendation in the top k (success rate). rec@k: the share of
    # the labels there (recall). r50 is the first stage's whole job, the label reaching the union
    return {"r5": int(any(hits[:5])), "r10": int(any(hits[:10])), "r50": int(any(hits[:50])),
            "rec10": sum(hits[:10]) / len(expected), "rec50": sum(hits[:50]) / len(expected),
            "p1": hits[0] if hits else 0,
            "mrr": 0.0 if first is None else 1.0 / (first + 1), "ndcg10": dcg / ideal if ideal else 0.0}


def make_reranker(entry: dict, precision: str, top_n: int, backend: str = "core"):
    # core is explicit pairs on ov.Core. genai is the GenAI pipeline, for parity checks only
    if backend == "core" and entry["architecture"] != "Qwen3ForCausalLM":
        from rerank_core import CoreReranker
        return CoreReranker(candidate_dir(entry["id"], precision), entry["max_length"])
    import openvino_genai as ov_genai
    config = ov_genai.TextRerankPipeline.Config()
    config.top_n = top_n
    config.max_length = entry["max_length"]
    if entry.get("padding_side"):
        config.padding_side = entry["padding_side"]
    return ov_genai.TextRerankPipeline(str(candidate_dir(entry["id"], precision)), DEVICE, config)


def rerank(pipe, entry: dict, query: str, texts: list[str]) -> list[tuple[int, float]]:
    if entry["architecture"] == "Qwen3ForCausalLM":
        query = f"{QWEN3_PREFIX}<Instruct>: {QWEN3_TASK}\n<Query>: {query}\n"
        texts = [f"<Document>: {t}{QWEN3_SUFFIX}" for t in texts]
    return pipe.rerank(query, texts)


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("id")
    ap.add_argument("--precision", default="fp16")
    ap.add_argument("--field", default="text", choices=["text", "text_prefixed"])
    ap.add_argument("--query-model", default=None, help="separate query tower (MedCPT)")
    ap.add_argument("--queries", default=None)
    ap.add_argument("--modes", default="sentence,note,both")
    ap.add_argument("--rerankers", default="")
    ap.add_argument("--rerank-precision", default="fp16")
    ap.add_argument("--rerank-backend", default="core", choices=["core", "genai"])
    ap.add_argument("--hybrid", default="off")
    ap.add_argument("--lexical-scope", default="drug", choices=["all", "drug"])
    ap.add_argument("--topk", type=int, default=50)
    ap.add_argument("--union", default="max", choices=["max", "rrf", "zmax"], help="how sub-query candidate lists merge")
    ap.add_argument("--note-weight", type=int, default=1, help="rrf only: votes cast by the whole-note query")
    ap.add_argument("--v1-baseline", action="store_true", help="whole note, no reranker, no threshold")
    ap.add_argument("--self-test", type=int, default=0, help="use N chunks' own text as queries")
    ap.add_argument("--dump-union", action="store_true", help="write every query's union with both scores")
    args = ap.parse_args()
    if args.v1_baseline:
        args.modes, args.rerankers, args.hybrid = "note", "", "off"
    return args


def load_queries(args, chunks: dict, ids: list[str]) -> list[dict]:
    if args.self_test:
        step = max(1, len(ids) // args.self_test)
        return [{"qid": f"self-{i}", "set": "self", "text": chunks[ids[i]]["text"], "mode": "sentence",
                 "expected_ids": [ids[i]], "expected_codes": [chunks[ids[i]]["code"]], "negative": False}
                for i in range(0, len(ids), step)][:args.self_test]
    qfile = args.queries or sorted((RESULTS / "queries").glob("queries-*.jsonl"))[-1]
    return read_jsonl(qfile)


def sub_queries(q: dict, mode: str) -> list[str]:
    """What is embedded for one query: its sentences, the whole text, or both, per the mode."""
    subqueries = [q["text"]] if mode == "note" or q["mode"] == "sentence" and mode != "both" else []
    if mode in ("sentence", "both") and q["mode"] == "note":
        subqueries += split_sentences(q["text"])
    if mode == "both" and q["text"] not in subqueries:
        subqueries.append(q["text"])
    if mode in ("sentence", "both"):
        subqueries += [s for s in q.get("extra_sentences", []) if s not in subqueries]
    return subqueries or [q["text"]]


def first_stage(scores, subqueries: list[str], whole: str, args, bm25, hybrid: str):
    """Merge the sub-queries' candidate lists into one union.

    Returns (union, cos_of, best): union is [(idx, (rule_score, trigger))] in rule order, cos_of the best
    cosine per candidate, best the full candidate dict the second stage reorders.
    """
    best, cos_of, lists = {}, {}, []
    for si, sub in enumerate(subqueries):
        order = np.argsort(-scores[si])[:args.topk]
        if hybrid == "on" and bm25 is not None:
            lex = lexical_query(sub, args.lexical_scope)
            if lex:
                lex_order = list(np.argsort(-bm25.get_scores(lex))[:args.topk])
                fused = rrf([list(order), lex_order])
                order = np.array(sorted(fused, key=fused.get, reverse=True)[:args.topk])
        lists.append([int(i) for i in order])
        top = scores[si][order]
        mu, sd = float(top.mean()), float(top.std()) or 1.0
        for idx in order:
            s = float(scores[si][idx])
            cos_of[int(idx)] = max(s, cos_of.get(int(idx), -1e9))
            # max takes the best cosine across sub-queries. zmax scores it within its own list
            rule_score = (s - mu) / sd if args.union == "zmax" else s
            if rule_score > best.get(int(idx), (-1e9, ""))[0]:
                best[int(idx)] = (rule_score, sub)
    if args.union == "rrf":
        # Rank vote across sub-queries. The whole-note query casts --note-weight votes
        votes = [l for l, sub in zip(lists, subqueries)
                 for _ in range(args.note_weight if sub == whole and len(subqueries) > 1 else 1)]
        fused = rrf(votes)
        best = {i: (fused[i], best[i][1]) for i in fused}
    union = sorted(best.items(), key=lambda kv: -kv[1][0])[:args.topk]
    return union, cos_of, best


def second_stage(union, reranker, chunks: dict, ids: list[str]) -> dict:
    """Rerank the union once, each candidate paired with the sub-query that found it."""
    rentry, rpipe = reranker
    groups = {}
    for idx, (_, trigger) in union:
        groups.setdefault(trigger, []).append(idx)
    rescored = {}
    for trigger, members in groups.items():
        for j, sc in rerank(rpipe, rentry, trigger, [chunks[ids[i]]["text"] for i in members]):
            rescored[members[j]] = (float(sc), trigger)
    return rescored


def record(q: dict, mode: str, hybrid: str, rname: str, ranked, union, cos_of, best, ids, codes, args) -> dict:
    ranked_ids = [ids[i] for i, _ in ranked]
    top_scores = [s for _, (s, _) in ranked[:args.topk]]
    m = metrics(ranked_ids, set(q["expected_ids"])) if not q["negative"] else {}
    entity = int(codes[ranked[0][0]] in set(q["expected_codes"])) if ranked and q["expected_codes"] else None
    rec = {"qid": q["qid"], "set": q["set"], "mode": mode, "hybrid": hybrid, "reranker": rname,
           "negative": q["negative"], "expected_ids": q["expected_ids"],
           "top": [(ids[i], round(s, 4)) for i, (s, _) in ranked[:10]]}
    if args.dump_union:
        # union with both scores, for offline fusion and threshold work
        rec["union"] = [{"id": ids[i], "cos": round(cos_of[i], 4), "first": round(c, 4), "score": round(best[i][0], 4)}
                        for i, (c, _) in union]
    rec.update({"trigger": ranked[0][1][1] if ranked else "", "entity_top1": entity, **m,
                "score_max": top_scores[0] if top_scores else None,
                "score_gap": (top_scores[0] - top_scores[1]) if len(top_scores) > 1 else None,
                "score_z": ((top_scores[0] - float(np.mean(top_scores))) / (float(np.std(top_scores)) + 1e-9))
                if len(top_scores) > 2 else None})
    return rec


def set_rows(args, mode: str, hybrid: str, rname: str, per_set: dict, elapsed: float) -> list[dict]:
    rows = []
    for sname, recs in per_set.items():
        pos = [r for r in recs if not r["negative"]]
        row = {"embedder": args.id, "precision": args.precision, "field": args.field, "mode": mode,
               "hybrid": hybrid, "reranker": rname, "set": sname, "n": len(pos), "negatives": len(recs) - len(pos),
               "seconds": round(elapsed, 1)}
        if pos:
            for k in METRIC_KEYS:
                row[k] = round(sum(r[k] for r in pos) / len(pos), 4)
            ent = [r["entity_top1"] for r in pos if r["entity_top1"] is not None]
            row["entity_top1"] = round(sum(ent) / len(ent), 4) if ent else ""
        rows.append(row)
    return rows


def threshold_sweep(per_query: list[dict], mode: str, hybrid: str, rname: str) -> list[dict]:
    """False-positive rate on the negatives against abstention on the positives, per score rule."""
    rows = []
    for rule in THRESHOLD_RULES:
        vals = [(r[rule], r["negative"]) for r in per_query
                if r["mode"] == mode and r["hybrid"] == hybrid and r["reranker"] == rname and r[rule] is not None]
        if not vals or not any(neg for _, neg in vals):
            continue
        for t in np.quantile([v for v, _ in vals], np.linspace(0, 1, 41)):
            fp = sum(1 for v, neg in vals if neg and v >= t) / max(1, sum(neg for _, neg in vals))
            abst = sum(1 for v, neg in vals if not neg and v < t) / max(1, sum(not neg for _, neg in vals))
            rows.append({"mode": mode, "hybrid": hybrid, "reranker": rname, "rule": rule,
                         "threshold": round(float(t), 5), "fp_rate": round(fp, 4), "abstention": round(abst, 4)})
    return rows


def write_outputs(out, args, rows: list[dict], threshold_rows: list[dict], per_query: list[dict]) -> None:
    # Written after every pass, so a long run is readable while it runs and a kill loses one pass
    with open(out / "metrics.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=sorted({k for r in rows for k in r}))
        w.writeheader()
        w.writerows(rows)
    if threshold_rows:
        with open(out / "thresholds.csv", "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=list(threshold_rows[0]))
            w.writeheader()
            w.writerows(threshold_rows)
    write_jsonl(out / "per_query.jsonl", per_query)
    with open(out / "summary.md", "w", encoding="utf-8") as f:
        f.write(f"# {args.id} {args.precision} {args.field}\n\n| set | mode | hybrid | reranker | n | s@5 | s@10 | s@50 | rec@10 | rec@50 | p@1 | mrr | ndcg@10 | entity | s |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
        for r in rows:
            f.write(f"| {r['set']} | {r['mode']} | {r['hybrid']} | {r['reranker']} | {r['n']} | {r.get('r5','')} | {r.get('r10','')} | {r.get('r50','')} | {r.get('rec10','')} | {r.get('rec50','')} | {r.get('p1','')} | {r.get('mrr','')} | {r.get('ndcg10','')} | {r.get('entity_top1','')} | {r['seconds']} |\n")


def main():
    args = parse_args()
    entry = candidate(args.id)
    emb_dir = RESULTS / "emb" / f"{args.id}-{args.precision}-{args.field}"
    docs = np.load(emb_dir / "docs.npy")
    meta = read_json(emb_dir / "meta.json")
    chunks = {c["id"]: c for c in read_jsonl(latest_chunks())}
    ids = meta["chunk_ids"]
    codes = np.array([chunks[cid]["code"] for cid in ids])
    queries = load_queries(args, chunks, ids)

    query_entry = candidate(args.query_model) if args.query_model else entry
    query_dir = candidate_dir(args.query_model or args.id, args.precision)
    qpipe = make_pipeline(query_entry, query_dir)

    bm25 = None
    if "on" in args.hybrid.split(","):
        from rank_bm25 import BM25Okapi
        bm25 = BM25Okapi([tokens(chunks[cid]["text"]) for cid in ids])

    rerankers = {rid: (candidate(rid), make_reranker(candidate(rid), args.rerank_precision, args.topk, args.rerank_backend))
                 for rid in args.rerankers.split(",") if rid}

    config = {"embedder": args.id, "precision": args.precision, "field": args.field, "query_model": args.query_model,
              "modes": args.modes, "rerankers": list(rerankers), "rerank_precision": args.rerank_precision,
              "rerank_backend": args.rerank_backend,
              "hybrid": args.hybrid, "lexical_scope": args.lexical_scope, "topk": args.topk,
              "union": args.union if args.union != "rrf" or args.note_weight == 1 else f"rrf-note{args.note_weight}",
              "v1_baseline": args.v1_baseline, "queries": len(queries), "emb_meta": meta}
    out = run_dir(f"eval-{args.id}", config)

    rows, per_query, threshold_rows = [], [], []
    for mode in args.modes.split(","):
        for hybrid in args.hybrid.split(","):
            for rname in ["none"] + list(rerankers):
                t_start = time.time()
                per_set = {}
                for q in queries:
                    subqueries = sub_queries(q, mode)
                    scores = embed_queries(qpipe, subqueries) @ docs.T
                    union, cos_of, best = first_stage(scores, subqueries, q["text"], args, bm25, hybrid)
                    best = second_stage(union, rerankers[rname], chunks, ids) if rname != "none" and union else dict(union)
                    ranked = sorted(best.items(), key=lambda kv: -kv[1][0])
                    rec = record(q, mode, hybrid, rname, ranked, union, cos_of, best, ids, codes, args)
                    per_query.append(rec)
                    per_set.setdefault(q["set"], []).append(rec)
                elapsed = time.time() - t_start
                rows += set_rows(args, mode, hybrid, rname, per_set, elapsed)
                threshold_rows += threshold_sweep(per_query, mode, hybrid, rname)
                log(f"{mode} hybrid={hybrid} reranker={rname}: {elapsed:.0f}s")
                write_outputs(out, args, rows, threshold_rows, per_query)
    log(f"done -> {out}")


if __name__ == "__main__":
    main()
