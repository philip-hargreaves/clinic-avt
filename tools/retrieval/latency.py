"""CPU latency and memory for embedders and rerankers.

  python latency.py --embedders bge-base,bge-large --rerankers minilm-l6,gte-reranker-modernbert --precision fp16,int8

Writes rag/results/<stamp>-latency/latency.csv. Medians of repeated runs.
"""

import argparse
import csv
import time

import numpy as np
import psutil

from common import candidate, candidate_dir, latest_chunks, log, median_time, read_jsonl, run_dir
from embed import make_pipeline
from evaluate import make_reranker, rerank

REPEATS = 7


def rss_mb() -> float:
    return psutil.Process().memory_info().rss / 1e6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--embedders", default="")
    ap.add_argument("--rerankers", default="")
    ap.add_argument("--precision", default="fp16,int8")
    ap.add_argument("--pairs", default="30,50")
    ap.add_argument("--rerank-backend", default="core", choices=["core", "genai"])
    args = ap.parse_args()

    chunks = read_jsonl(latest_chunks())
    texts = [c["text"] for c in chunks]
    sentences = [t.split(". ")[0] for t in texts[:50]]
    note = " ".join(sentences[:30])
    rows = []
    out = run_dir("latency", vars(args))

    for cid in [c for c in args.embedders.split(",") if c]:
        for precision in args.precision.split(","):
            entry = candidate(cid)
            before = rss_mb()
            t0 = time.perf_counter()
            pipe = make_pipeline(entry, candidate_dir(cid, precision))
            load_s = time.perf_counter() - t0
            pipe.embed_query(sentences[0])
            per_sentence = median_time(lambda: pipe.embed_query(sentences[1]), REPEATS)
            per_note_seq = median_time(lambda: [pipe.embed_query(s) for s in sentences[:30]], 3)
            per_note_batch = median_time(lambda: pipe.embed_documents(sentences[:30]), 3)
            whole_note = median_time(lambda: pipe.embed_query(note), REPEATS)
            dims = entry["dims"]
            n = len(texts)
            mat = np.random.default_rng(0).standard_normal((n, dims), dtype=np.float32)
            mat /= np.linalg.norm(mat, axis=1, keepdims=True)
            q = mat[0]
            scan_fp32 = median_time(lambda: np.argpartition(-(mat @ q), 50)[:50], 20)
            rows.append({"role": "embedder", "id": cid, "precision": precision, "load_s": round(load_s, 2),
                         "rss_mb": round(rss_mb() - before), "per_sentence_ms": round(per_sentence * 1e3, 1),
                         "note30_sequential_ms": round(per_note_seq * 1e3), "note30_batched_ms": round(per_note_batch * 1e3),
                         "whole_note_ms": round(whole_note * 1e3, 1), "scan_fp32_ms": round(scan_fp32 * 1e3, 1),
                         "vectors": n, "dims": dims})
            log(str(rows[-1]))
            pipe = None  # frees the model before the next one loads

    for rid in [r for r in args.rerankers.split(",") if r]:
        for precision in args.precision.split(","):
            entry = candidate(rid)
            before = rss_mb()
            t0 = time.perf_counter()
            pipe = make_reranker(entry, precision, 50, args.rerank_backend)
            load_s = time.perf_counter() - t0
            rerank(pipe, entry, sentences[0], texts[:5])
            row = {"role": "reranker", "id": rid, "precision": precision, "backend": args.rerank_backend,
                   "load_s": round(load_s, 2), "rss_mb": round(rss_mb() - before)}
            for pairs in [int(p) for p in args.pairs.split(",")]:
                t = median_time(lambda: rerank(pipe, entry, sentences[0], texts[:pairs]), 5)
                row[f"rerank_{pairs}_ms"] = round(t * 1e3)
            rows.append(row)
            log(str(row))
            pipe = None  # frees the model before the next one loads

    with open(out / "latency.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=sorted({k for r in rows for k in r}))
        w.writeheader()
        w.writerows(rows)
    log(f"done -> {out}")


if __name__ == "__main__":
    main()
