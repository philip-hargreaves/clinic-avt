"""Measures each candidate's cold start, warm seconds per sheet, peak memory and size.

    python cost.py [--threads 8] [--sheets 5]

Each model runs in a fresh process. Cold start is load plus the first sheet, warm is the median
over the next sheets in Urdu and Polish, memory is the peak working set. Writes results/cost.json.
"""

import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from study import ROOT, option  # noqa: E402

PYTHON = sys.executable
LANGUAGES = ["Urdu", "Polish"]


def measure(name: str, threads: int, count: int) -> dict:
    """Runs in the child process."""
    import psutil
    from translate import Translator, load_set
    started = time.time()
    # Imported only to time Python import cost, which the engine does not pay
    import optimum.intel  # noqa: F401
    import transformers  # noqa: F401
    imports = time.time() - started
    sheets = [s for s in load_set("sheets", count + 1) if s["language"] == "Urdu"]
    started = time.time()
    translator = Translator(name, "int8", threads)
    load = time.time() - started
    started = time.time()
    translator.document(sheets[0]["source"], "Urdu")
    first = time.time() - started
    warm = {}
    for language in LANGUAGES:
        laps = []
        for sheet in sheets[1:]:
            started = time.time()
            translator.document(sheet["source"], language)
            laps.append(time.time() - started)
        laps.sort()
        warm[language] = laps[len(laps) // 2]
    words = sum(len(s["source"].split()) for s in sheets[1:]) / len(sheets[1:])
    process = psutil.Process()
    size = sum(p.stat().st_size for p in (ROOT / "models" / f"{name}-int8").rglob("*") if p.is_file())
    return {"imports_s": round(imports, 1), "load_s": round(load, 1), "first_sheet_s": round(first, 1),
            "cold_s": round(load + first, 1),
            "warm_s": {k: round(v, 1) for k, v in warm.items()}, "words_per_sheet": round(words),
            "peak_working_set_mb": round(process.memory_info().peak_wset / 2**20),
            "disk_mb": round(size / 2**20), "threads": threads}


def main():
    if "--child" in sys.argv:
        name = option("--child", "")
        threads, count = int(option("--threads", "8")), int(option("--sheets", "5"))
        print(json.dumps(measure(name, threads, count)))
        return
    threads, count = option("--threads", "8"), option("--sheets", "5")
    names = [p.stem[:-5] for p in sorted((ROOT / "results" / "sheets").glob("*-int8.jsonl"))]
    results = {}
    for name in names:
        out = subprocess.run([PYTHON, __file__, "--child", name, "--threads", threads, "--sheets", count],
                             capture_output=True, text=True, encoding="utf-8")
        line = out.stdout.strip().splitlines()[-1] if out.stdout.strip() else ""
        try:
            results[name] = json.loads(line)
        except json.JSONDecodeError:
            results[name] = {"error": out.stderr[-800:]}
        print(name, results[name], flush=True)
    json.dump(results, open(ROOT / "results" / "cost.json", "w", encoding="utf-8"), indent=1)


if __name__ == "__main__":
    main()
