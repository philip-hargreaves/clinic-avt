"""Decode rate of one note model on the exact IR, one configuration per process (fresh GPU state).

The prompt is the narrative style, a long rheumatology consultation and the standard detail
file, in the model's template from eval/config.toml; greedy. The first generation warms, the
rest count. Prints one JSON line.

    python eval/performance/decode_bench.py qwen3.5-9b-int4 [--tokens 300] [--reps 2]
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.llm import NoteModel  # noqa: E402


def prompt_text():
    prompts = config.path("prompts")
    style = (prompts / "note-narrative.md").read_text(encoding="utf-8")
    detail = (prompts / "detail-concise.md").read_text(encoding="utf-8")
    transcript = config.path("sample_consult").read_text(encoding="utf-8")
    return style + "\n\nTRANSCRIPT:\n" + transcript + "\n" + detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model", help="a model name from eval/config.toml")
    ap.add_argument("--device", default="GPU")
    ap.add_argument("--tokens", type=int, default=300)
    ap.add_argument("--reps", type=int, default=2)
    args = ap.parse_args()

    import psutil
    model = NoteModel(args.model, args.device)
    prompt = prompt_text()
    runs = []
    text = ""
    for rep in range(args.reps):
        text, metrics = model.generate(prompt, args.tokens)
        runs.append({"rep": rep, **metrics})
    print(json.dumps({
        "model": args.model,
        "pipeline": model.spec["pipeline"],
        "template": model.spec["template"],
        "properties": model.spec["properties"],
        "device": args.device,
        "load_s": round(model.load_s, 1),
        "peak_rss_gb": round(psutil.Process().memory_info().peak_wset / 2**30, 2),
        "runs": runs,
        "text_head": text[:160].replace("\n", " "),
    }))


if __name__ == "__main__":
    main()
