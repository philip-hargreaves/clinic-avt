"""Runs the translator's lifecycle in the real engine, driven over the pipe like the shell.

    python lifecycle_smoke.py <work dir> [wav]

Starts a fresh engine on a throwaway store seeded with the demo sessions. Reopening a session
with a sheet should warm the translator, Translate should stream at once and a capture start
should release it. Prints the timings and the engine's memory at each step. Needs the GPU free
and the release engine built.
"""

import json
import os
import sys
import time
from pathlib import Path

WORK = Path(sys.argv[1])
WAV = sys.argv[2] if len(sys.argv) > 2 else None
os.environ["EVAL_PERF_LOOP"] = str(WORK)  # the engine's store and logs go to the work dir
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from performance import perf_loop as pl  # noqa: E402


def wait_log(engine, needle, timeout):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        for line in engine.new_log_lines():
            if needle in line:
                return time.perf_counter() - t0, line.strip()
        time.sleep(0.05)
    return None, None


def settled(engine):
    """The engine's memory once background loads have finished."""
    last, still = None, 0
    for _ in range(120):
        now = pl.process_memory_mb(engine.proc.pid)
        if last is not None and abs(now["private_mb"] - last["private_mb"]) < 15:
            still += 1
            if still >= 4:
                return now
        else:
            still = 0
        last = now
        time.sleep(1)
    return last


def main():
    os.makedirs(pl.STORE, exist_ok=True)
    os.makedirs(pl.LOGS, exist_ok=True)
    engine = pl.start_engine(0)
    report = {}
    try:
        engine.new_log_lines()
        engine.request("demo/seed", None, 60)
        session = engine.request("session/list", None, 15)["sessions"][0]["id"]
        report["mem_before"] = settled(engine)

        t0 = time.perf_counter()
        engine.request("session/open", {"id": session}, 15)
        report["open_rpc_s"] = round(time.perf_counter() - t0, 3)
        warmed, line = wait_log(engine, "translator warmed", 60)
        report["open_to_warmed_s"] = warmed and round(warmed, 2)
        report["warm_line"] = line
        report["mem_warm"] = settled(engine)

        t0 = time.perf_counter()
        engine.request("patient/translate", {"id": session, "language": "Polish"}, 15)
        first = ready = None
        while ready is None and time.perf_counter() - t0 < 120:
            got = engine.next_notification(1)
            if got is None:
                continue
            method = got[1].get("method")
            if method == "translate/partial" and first is None:
                first = time.perf_counter() - t0
            if method in ("translate/ready", "translate/failed"):
                ready = time.perf_counter() - t0
                report["outcome"] = method
        report["translate_first_partial_s"] = first and round(first, 2)
        report["translate_ready_s"] = ready and round(ready, 2)
        report["mem_translated"] = settled(engine)
        engine.request("session/close", None, 15)

        if WAV:
            engine.request("session/start", {"replay": {"path": WAV, "speed": 8.0}}, 60)
            released, line = wait_log(engine, "translator released", 30)
            report["start_to_released_s"] = released and round(released, 2)
            engine.request("session/cancel", None, 60)
            report["mem_released"] = settled(engine)
    finally:
        engine.close()
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
