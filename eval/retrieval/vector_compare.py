"""Exact scan against index libraries at corpus size.

  python vector_compare.py [--emb rag/results/emb/<run>/docs.npy] [--sizes 23207,100000] [--k 10]

Writes rag/results/<stamp>-vector-compare/vector-compare.csv
"""

import argparse
import csv
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import log  # noqa: E402
from harness import median_time, run_dir  # noqa: E402

QUERIES = 100


def recall(found: np.ndarray, truth: np.ndarray) -> float:
    return float(np.mean([len(set(f) & set(t)) / len(t) for f, t in zip(found, truth)]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emb", default=None)
    ap.add_argument("--sizes", default="23207,100000")
    ap.add_argument("--dims", type=int, default=1024)
    ap.add_argument("--k", type=int, default=10)
    args = ap.parse_args()

    rng = np.random.default_rng(0)
    base = np.load(args.emb).astype(np.float32) if args.emb else None
    out = run_dir("vector-compare", vars(args))
    rows = []
    for n in [int(s) for s in args.sizes.split(",")]:
        if base is not None and len(base) >= n:
            mat = base[:n]
        else:
            src = base if base is not None else rng.standard_normal((1000, args.dims), dtype=np.float32)
            mat = src[rng.integers(0, len(src), n)] + 0.05 * rng.standard_normal((n, src.shape[1]), dtype=np.float32)
        mat /= np.linalg.norm(mat, axis=1, keepdims=True)
        dims = mat.shape[1]
        queries = mat[rng.integers(0, n, QUERIES)] + 0.02 * rng.standard_normal((QUERIES, dims), dtype=np.float32)
        queries /= np.linalg.norm(queries, axis=1, keepdims=True)
        truth = np.argsort(-(queries @ mat.T), axis=1)[:, :args.k]
        qi = iter(range(QUERIES * 10))

        def exact_fp32():
            q = queries[next(qi) % QUERIES]
            return np.argpartition(-(mat @ q), args.k)[:args.k]
        rows.append({"n": n, "dims": dims, "method": "numpy fp32 exact", "ms": round(median_time(exact_fp32, QUERIES) * 1e3, 2), "recall": 1.0})

        # Per-vector scale, since unit-vector components sit near 1/sqrt(d)
        d_scale = np.abs(mat).max(axis=1, keepdims=True) / 127.0
        mat_i8 = np.round(mat / d_scale).astype(np.int8)
        mat_i32 = mat_i8.astype(np.int32)
        q_scale = np.abs(queries).max(axis=1, keepdims=True) / 127.0
        q_i8 = np.round(queries / q_scale).astype(np.int8)
        found = np.array([np.argsort(-((mat_i32 @ q.astype(np.int32)) * d_scale[:, 0]))[:args.k] for q in q_i8])

        def exact_i8():
            q = q_i8[next(qi) % QUERIES].astype(np.int32)
            return np.argpartition(-((mat_i32 @ q) * d_scale[:, 0]), args.k)[:args.k]
        rows.append({"n": n, "dims": dims, "method": "numpy int8 exact, per-vector scale", "ms": round(median_time(exact_i8, QUERIES) * 1e3, 2), "recall": round(recall(found, truth), 4)})

        try:
            import hnswlib
            idx = hnswlib.Index(space="ip", dim=dims)
            t0 = time.perf_counter()
            idx.init_index(max_elements=n, ef_construction=200, M=16)
            idx.add_items(mat)
            build = time.perf_counter() - t0
            idx.set_ef(50)
            labels, _ = idx.knn_query(queries, k=args.k)
            rows.append({"n": n, "dims": dims, "method": "hnswlib M16 ef50", "ms": round(median_time(lambda: idx.knn_query(queries[next(qi) % QUERIES], k=args.k), QUERIES) * 1e3, 2),
                         "recall": round(recall(labels, truth), 4), "build_s": round(build, 1)})
        except Exception as e:
            log(f"hnswlib skipped: {e}")

        try:
            from usearch.index import Index
            idx = Index(ndim=dims, metric="ip", dtype="i8")
            t0 = time.perf_counter()
            idx.add(np.arange(n), mat)
            build = time.perf_counter() - t0
            matches = idx.search(queries, args.k)
            labels = np.array([m.keys for m in matches]) if hasattr(matches[0], "keys") else matches.keys
            rows.append({"n": n, "dims": dims, "method": "usearch i8 hnsw", "ms": round(median_time(lambda: idx.search(queries[next(qi) % QUERIES], args.k), QUERIES) * 1e3, 2),
                         "recall": round(recall(labels, truth), 4), "build_s": round(build, 1)})
        except Exception as e:
            log(f"usearch skipped: {e}")

        try:
            import sqlite3
            import sqlite_vec
            db = sqlite3.connect(":memory:")
            db.enable_load_extension(True)
            sqlite_vec.load(db)
            db.execute(f"create virtual table v using vec0(e float[{dims}])")
            t0 = time.perf_counter()
            db.executemany("insert into v(rowid, e) values (?, ?)", [(i, mat[i].tobytes()) for i in range(n)])
            build = time.perf_counter() - t0

            def sv():
                q = queries[next(qi) % QUERIES]
                return db.execute("select rowid from v where e match ? order by distance limit ?", (q.tobytes(), args.k)).fetchall()
            labels = np.array([[r[0] for r in db.execute("select rowid from v where e match ? order by distance limit ?", (q.tobytes(), args.k)).fetchall()] for q in queries])
            rows.append({"n": n, "dims": dims, "method": "sqlite-vec vec0 float", "ms": round(median_time(sv, QUERIES) * 1e3, 2),
                         "recall": round(recall(labels, truth), 4), "build_s": round(build, 1)})
        except Exception as e:
            log(f"sqlite-vec skipped: {e}")
        log(f"n={n} done")

    with open(out / "vector-compare.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=sorted({k for r in rows for k in r}))
        w.writeheader()
        w.writerows(rows)
    for r in rows:
        log(str(r))
    log(f"done -> {out}")


if __name__ == "__main__":
    main()
