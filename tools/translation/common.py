"""Shared paths, constants and IO for the translation study."""

import json
import os
import sys
from pathlib import Path

ROOT = Path(os.environ.get("MT_ROOT", r"D:\clinicavt-mt"))
APP_MODELS = Path(__file__).resolve().parents[2] / "models"
LANGUAGES = ["Urdu", "Punjabi", "Bengali", "Gujarati", "Polish", "Romanian", "Arabic", "Somali"]
LOW_RESOURCE = ("Urdu", "Punjabi", "Bengali", "Gujarati", "Somali")
REFERENCE = "nllb-600m-int8"
BOOTSTRAP = 2000


def option(flag: str, default: str) -> str:
    return sys.argv[sys.argv.index(flag) + 1] if flag in sys.argv else default


def force_ipv4() -> None:
    # IPv6 drops on this network
    import socket
    lookup = socket.getaddrinfo
    socket.getaddrinfo = lambda host, port, family=0, *rest, **more: lookup(
        host, port, socket.AF_INET, *rest, **more)


def read_jsonl(path: Path) -> list[dict]:
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def bootstrap_interval(rng, delta) -> tuple[float, float]:
    """95% paired bootstrap interval of the mean of `delta`."""
    import numpy as np
    draws = rng.integers(0, len(delta), size=(BOOTSTRAP, len(delta)))
    low, high = np.percentile(delta[draws].mean(axis=1), [2.5, 97.5])
    return low, high
