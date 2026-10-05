"""Prompt and temperature grid for the concise note. One tier per call, with the model loaded once.

Prompts: the shipped files, a simple two-sentence prompt and a minimal one-line prompt
(prompts/grid-simple, prompts/grid-minimal, each replacing note-narrative.md and
detail-concise.md). Temperatures: greedy, 0.3 and 0.7 with top-p 0.95. Sampled arms take one draw on
every consultation and extra draws on the first --draw-consults, for run-to-run variation.

    python evaluation/summarisation/grid.py --tier default [--draws 3 --draw-consults 10] [--limit N]

Writes notes/grid-<tier>-<prompt>-<temp>-d<draw>/<cid>.md and notes/grid-timings.jsonl. Shipped
greedy notes are reused from the tier's existing concise notes when --reuse names them.
"""
import argparse
import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import append_jsonl  # noqa: E402
from summarisation import generate  # noqa: E402

PROMPTS = {
    "shipped": None,
    "simple": Path(__file__).parent / "prompts" / "grid-simple",
    "minimal": Path(__file__).parent / "prompts" / "grid-minimal",
}
TEMPS = [None, 0.3, 0.7]
TOP_P = 0.95


def compose(prompt_dir, turns):
    generate.PROMPT_OVERLAY = prompt_dir
    style = generate.app_prompt("note-narrative.md")
    length = generate.app_prompt("detail-concise.md")
    return style + generate.transcript_block(turns) + "\n" + length


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", required=True, choices=["constrained", "default", "accuracy"])
    ap.add_argument("--prompts", default=",".join(PROMPTS))
    ap.add_argument("--temps", default="greedy,0.3,0.7", help="subset of greedy,0.3,0.7")
    ap.add_argument("--draws", type=int, default=3)
    ap.add_argument("--draw-consults", type=int, default=10)
    ap.add_argument("--reuse", help="notes folder holding this tier's shipped greedy concise notes")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    from common.llm import NoteModel
    name = config.section("summarisation")["tiers"][args.tier]
    items = generate.sealed_transcripts()[: args.limit or None]
    timings = generate.data() / "notes" / "grid-timings.jsonl"
    model = None
    for prompt in args.prompts.split(","):
        for temp in TEMPS:
            if ("greedy" if temp is None else str(temp)) not in args.temps.split(","):
                continue
            draws = 1 if temp is None else args.draws
            for draw in range(1, draws + 1):
                tag = f"grid-{args.tier}-{prompt}-{'greedy' if temp is None else temp}-d{draw}"
                out = generate.notes_dir(tag)
                for index, (cid, turns) in enumerate(items):
                    if draw > 1 and index >= args.draw_consults:
                        break
                    path = out / f"{cid}.md"
                    if path.exists():
                        continue
                    if prompt == "shipped" and temp is None and args.reuse:
                        src = generate.data() / "notes" / args.reuse / f"{cid}.md"
                        if src.exists():
                            shutil.copy2(src, path)
                            continue
                    if model is None:
                        model = NoteModel(name)
                        print(f"{name} loaded in {model.load_s:.1f} s", flush=True)
                        append_jsonl(timings, {"tier": args.tier, "event": "load", "seconds": round(model.load_s, 1)})
                    text, m = model.generate(compose(PROMPTS[prompt], turns), 1024, temperature=temp,
                                             top_p=TOP_P if temp is not None else None,
                                             seed=draw if temp is not None else None)
                    path.write_text(text, encoding="utf-8")
                    append_jsonl(timings, {"tag": tag, "tier": args.tier, "prompt": prompt, "temperature": temp,
                                           "draw": draw, "consult": cid, "words": len(text.split()),
                                           "refused": text.startswith(generate.REFUSAL), **m})
                print(f"[{tag}] done", flush=True)


if __name__ == "__main__":
    main()
