"""Shared paths and IO for the retrieval selection harness."""

import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
RAG = REPO / "rag"
SOURCES = RAG / "sources"
GOLD = RAG / "gold"
CANDIDATES = RAG / "candidates"
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


def force_ipv4() -> None:
    # IPv6 drops on this network. RETRIEVAL_IPV6=1 disables the workaround
    if os.environ.get("RETRIEVAL_IPV6") == "1":
        return
    import socket
    original = socket.getaddrinfo

    def ipv4_first(*args, **kwargs):
        results = original(*args, **kwargs)
        return [r for r in results if r[0] == socket.AF_INET] or results

    socket.getaddrinfo = ipv4_first


force_ipv4()


def log(msg: str) -> None:
    print(time.strftime("%H:%M:%S"), msg, file=sys.stderr, flush=True)


def read_jsonl(path: Path) -> list[dict]:
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def write_jsonl(path: Path, rows) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    n = 0
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
            n += 1
    return n


def read_json(path: Path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)


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
    # Reading order. -layout interleaves columns
    return subprocess.run([exe, str(pdf), "-"], capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout
