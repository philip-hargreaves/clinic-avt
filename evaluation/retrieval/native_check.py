"""Parity of the native C++ pipelines against the Python harness and the reference model.

  python native_check.py bge-base --precision fp16 [--proof <proof.exe>] [--n 20]
  python native_check.py minilm-l6 --precision fp16 --rerank

Writes rag/results/<stamp>-native-<id>/parity.json
"""

import argparse
import json
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import log, read_jsonl, write_json  # noqa: E402
from harness import REPO, candidate, candidate_dir, latest_chunks, run_dir  # noqa: E402
from embed import make_pipeline
from evaluate import make_reranker, rerank

OPENVINO_BIN = [REPO / "external" / "openvino" / "runtime" / "bin" / "intel64" / "Release",
                REPO / "external" / "openvino" / "runtime" / "3rdparty" / "tbb" / "bin"]
POOLING_ARG = {"cls": "cls", "mean": "mean", "last_token": "last"}


def run_proof(proof: str, args: list[str]) -> list[str]:
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(str(p) for p in OPENVINO_BIN) + os.pathsep + env["PATH"]
    result = subprocess.run([proof, *args], capture_output=True, text=True, encoding="utf-8", env=env)
    if result.returncode != 0:
        raise SystemExit(f"proof failed: {result.stderr}")
    return [line for line in result.stdout.splitlines() if line.strip()]


def cosine_rows(a: np.ndarray, b: np.ndarray) -> dict:
    a = a / np.linalg.norm(a, axis=1, keepdims=True)
    b = b / np.linalg.norm(b, axis=1, keepdims=True)
    cos = np.sum(a * b, axis=1)
    return {"n": int(len(cos)), "min": float(cos.min()), "median": float(np.median(cos))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("id")
    ap.add_argument("--precision", default="fp16")
    ap.add_argument("--proof", default=str(config.path("rag_native")))
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--rerank", action="store_true")
    args = ap.parse_args()

    entry = candidate(args.id)
    model_dir = candidate_dir(args.id, args.precision)
    chunks = read_jsonl(latest_chunks())
    texts = [c["text"].replace("\n", " ") for c in chunks[: args.n]]
    out = run_dir(f"native-{args.id}", vars(args))
    texts_file = out / "texts.txt"
    texts_file.write_text("\n".join(texts) + "\n", encoding="utf-8")
    report = {"id": args.id, "precision": args.precision, "n": len(texts)}

    if args.rerank:
        query = texts[0].split(". ")[0]
        native = [json.loads(line) for line in run_proof(args.proof, ["rerank", str(model_dir), str(entry["max_length"]), query, str(texts_file)])]
        pipe = make_reranker(entry, args.precision, len(texts), "genai")
        python = rerank(pipe, entry, query, texts)
        native_scores = {int(i): float(s) for i, s in native}
        python_scores = {int(i): float(s) for i, s in python}
        diffs = [abs(native_scores[i] - python_scores[i]) for i in native_scores if i in python_scores]
        report["rerank"] = {"pairs": len(diffs), "max_abs_score_diff": max(diffs) if diffs else None,
                            "same_top1": (max(native_scores, key=native_scores.get) == max(python_scores, key=python_scores.get))}
    else:
        proof_args = ["embed", str(model_dir), POOLING_ARG[entry["pooling"]], str(entry["max_length"]), str(texts_file)]
        if entry.get("query_instruction"):
            proof_args.append(entry["query_instruction"])
        lines = run_proof(args.proof, proof_args)
        split = lines.index("query")
        native_docs = np.array([json.loads(l) for l in lines[:split]], dtype=np.float32)
        native_query = np.array(json.loads(lines[split + 1]), dtype=np.float32)
        pipe = make_pipeline(entry, model_dir)
        python_docs = np.asarray(pipe.embed_documents(texts), dtype=np.float32)
        python_query = np.asarray(pipe.embed_query(texts[0]), dtype=np.float32)
        report["native_vs_python_docs"] = cosine_rows(native_docs, python_docs)
        report["native_vs_python_query"] = float(np.dot(native_query, python_query) / (np.linalg.norm(native_query) * np.linalg.norm(python_query)))
        try:
            from sentence_transformers import SentenceTransformer
            ref = SentenceTransformer(entry["hf"], device="cpu").encode(texts, normalize_embeddings=True, convert_to_numpy=True).astype(np.float32)
            report["native_vs_reference_docs"] = cosine_rows(native_docs, ref)
        except Exception as e:  # Reference optional
            report["native_vs_reference_docs"] = {"error": str(e)}
    write_json(out / "parity.json", report)
    log(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
