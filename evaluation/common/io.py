"""JSON and JSON-lines files, and the timestamped log line the harnesses print."""

import json
import sys
import time
from pathlib import Path


def log(msg: str) -> None:
    print(time.strftime("%H:%M:%S"), msg, file=sys.stderr, flush=True)


def read_json(path: Path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_json(path: Path, obj, indent=2) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=indent, ensure_ascii=False)


def read_jsonl(path: Path) -> list[dict]:
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def write_jsonl(path: Path, rows) -> int:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    n = 0
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
            n += 1
    return n


def append_jsonl(path: Path, row: dict) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")


def force_ipv4() -> None:
    # IPv6 drops on this network; EVAL_IPV6=1 leaves name lookup alone
    import os
    import socket
    if os.environ.get("EVAL_IPV6") == "1":
        return
    original = socket.getaddrinfo

    def ipv4_first(*args, **kwargs):
        results = original(*args, **kwargs)
        return [r for r in results if r[0] == socket.AF_INET] or results

    socket.getaddrinfo = ipv4_first
