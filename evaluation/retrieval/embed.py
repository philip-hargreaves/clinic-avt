"""Embed a chunk file with one exported candidate on CPU.

  python embed.py bge-base --precision int8 [--field text|text_prefixed] [--reference]

Writes rag/results/emb/<candidate>-<precision>-<field>/{docs.npy, meta.json, faithfulness.json}
"""

import argparse
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import log, read_jsonl, write_json  # noqa: E402
from harness import DEVICE, RESULTS, candidate, candidate_dir, latest_chunks, plugin_properties  # noqa: E402

POOLING = {"cls": "CLS", "mean": "MEAN", "last_token": "LAST_TOKEN"}


def make_pipeline(entry: dict, model_dir, max_length: int | None = None):
    import openvino_genai as ov_genai

    config = ov_genai.TextEmbeddingPipeline.Config()
    config.pooling_type = getattr(ov_genai.TextEmbeddingPipeline.PoolingType, POOLING[entry["pooling"]])
    config.normalize = True
    config.max_length = max_length or entry["max_length"]
    if entry.get("query_instruction"):
        config.query_instruction = entry["query_instruction"]
    if entry.get("document_instruction"):
        config.embed_instruction = entry["document_instruction"]
    if entry.get("padding_side"):
        config.padding_side = entry["padding_side"]
    return ov_genai.TextEmbeddingPipeline(str(model_dir), DEVICE, config, **plugin_properties())


def embed_documents(pipe, texts: list[str], batch: int = 32) -> np.ndarray:
    out = []
    t0 = time.time()
    for i in range(0, len(texts), batch):
        out.extend(pipe.embed_documents(texts[i:i + batch]))
        if (i // batch) % 50 == 0:
            log(f"  {i + batch}/{len(texts)} {time.time() - t0:.0f}s")
    return np.asarray(out, dtype=np.float32)


def embed_queries(pipe, texts: list[str]) -> np.ndarray:
    return np.asarray([pipe.embed_query(t) for t in texts], dtype=np.float32)


def over_length(model_dir, texts: list[str], max_length: int) -> int:
    from transformers import AutoTokenizer
    tok = AutoTokenizer.from_pretrained(str(model_dir))
    return sum(len(tok(t, add_special_tokens=True)["input_ids"]) > max_length for t in texts)


def faithfulness(entry: dict, texts: list[str], ours: np.ndarray) -> dict:
    from sentence_transformers import SentenceTransformer
    ref = SentenceTransformer(entry["hf"], device="cpu")
    prefixed = [entry.get("document_instruction", "") + t for t in texts]
    ref_emb = ref.encode(prefixed, normalize_embeddings=True, convert_to_numpy=True).astype(np.float32)
    if ref_emb.shape[1] != ours.shape[1]:
        return {"error": f"dims differ: reference {ref_emb.shape[1]} vs ours {ours.shape[1]}"}
    cos = np.sum(ref_emb * ours, axis=1)
    return {"n": len(texts), "cosine_min": float(cos.min()), "cosine_median": float(np.median(cos)),
            "cosine_mean": float(cos.mean())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("id")
    ap.add_argument("--precision", default="fp16")
    ap.add_argument("--field", default="text", choices=["text", "text_prefixed"])
    ap.add_argument("--chunks", default=None)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--reference", action="store_true", help="compare 200 chunks against sentence-transformers")
    args = ap.parse_args()

    entry = candidate(args.id)
    model_dir = candidate_dir(args.id, args.precision)
    chunks = read_jsonl(args.chunks) if args.chunks else read_jsonl(latest_chunks())
    if args.limit:
        chunks = chunks[:args.limit]
    texts = [c[args.field] for c in chunks]
    out = RESULTS / "emb" / f"{args.id}-{args.precision}-{args.field}"
    out.mkdir(parents=True, exist_ok=True)

    log(f"loading {model_dir}")
    t0 = time.time()
    pipe = make_pipeline(entry, model_dir)
    load_s = time.time() - t0
    log(f"loaded in {load_s:.1f}s; embedding {len(texts)} chunks")
    t0 = time.time()
    docs = embed_documents(pipe, texts)
    embed_s = time.time() - t0
    np.save(out / "docs.npy", docs)
    truncated = over_length(model_dir, texts, entry["max_length"])
    meta = {"id": args.id, "precision": args.precision, "field": args.field, "chunks": len(texts),
            "dims": int(docs.shape[1]), "load_s": round(load_s, 2), "embed_s": round(embed_s, 1),
            "chunks_per_s": round(len(texts) / embed_s, 1), "over_max_length": truncated,
            "chunk_ids": [c["id"] for c in chunks]}
    write_json(out / "meta.json", meta)
    log(f"{len(texts)} chunks in {embed_s:.0f}s ({meta['chunks_per_s']}/s); {truncated} over max_length")
    if args.reference:
        sample = list(range(0, len(texts), max(1, len(texts) // 200)))[:200]
        result = faithfulness(entry, [texts[i] for i in sample], docs[sample])
        write_json(out / "faithfulness.json", result)
        log(f"faithfulness {json.dumps(result)}")


if __name__ == "__main__":
    main()
