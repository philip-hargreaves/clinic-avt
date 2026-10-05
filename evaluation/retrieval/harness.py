"""Paths, candidates and run directories of the retrieval selection harness."""

import os
import statistics
import subprocess
import time
from pathlib import Path

from common import config
from common.io import force_ipv4, log, read_json, write_json

HERE = Path(__file__).resolve().parent
REPO = config.REPO
RAG = config.path("rag")
SOURCES = RAG / "sources"
GOLD = RAG / "gold"
CANDIDATES = config.path("rag_candidates")
RESULTS = RAG / "results"
NICE_JSON = SOURCES / "nice" / "out" / "json"
NICE_MANIFEST = SOURCES / "nice" / "out" / "manifest.json"
CLIENT_PDFS = SOURCES / "st-georges" / "folder"
DEVICE = "CPU"


def plugin_properties() -> dict:
    # Harness processes share the CPU. A per-process thread cap stops them starving each other
    threads = os.environ.get("RETRIEVAL_THREADS")
    return {"INFERENCE_NUM_THREADS": int(threads)} if threads else {}


# Model downloads land beside the exports to keep them off C:
os.environ.setdefault("HF_HOME", str(CANDIDATES / ".hf"))
force_ipv4()


def load_shortlist() -> list[dict]:
    return read_json(HERE / "shortlist.json")


def candidate(cid: str) -> dict:
    for entry in load_shortlist():
        if entry["id"] == cid:
            return entry
    raise SystemExit(f"unknown candidate: {cid}")


def candidate_dir(cid: str, precision: str) -> Path:
    return CANDIDATES / f"{cid}-{precision}"


def run_dir(name: str, config: dict) -> Path:
    path = RESULTS / f"{time.strftime('%Y%m%d-%H%M%S')}-{name}"
    path.mkdir(parents=True, exist_ok=False)
    write_json(path / "config.json", config)
    log(f"run dir {path}")
    return path


def latest_chunks() -> Path:
    files = sorted((RESULTS / "chunks").glob("nice-*.jsonl"))
    if not files:
        raise SystemExit("no chunk file; run chunk.py first")
    return files[-1]


def median_time(fn, repeats: int) -> float:
    times = []
    for _ in range(repeats):
        t0 = time.perf_counter()
        fn()
        times.append(time.perf_counter() - t0)
    return statistics.median(times)


def pdftotext(pdf: Path, exe: str) -> str:
    # Reading order, since -layout interleaves columns
    return subprocess.run([exe, str(pdf), "-"], capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout
