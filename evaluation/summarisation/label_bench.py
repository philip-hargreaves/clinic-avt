# Runs the label prompt over a sweep's saved notes on one note model, so every title can be
# read before the prompt ships. Reads the transcripts perf_loop.py saves with PERF_SAVE and
# writes labels-<tag>.txt beside them. One GPU job.
#   python evaluation/summarisation/label_bench.py <tag> [model]
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.llm import NoteModel  # noqa: E402


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "sweep20"
    model = NoteModel(sys.argv[2] if len(sys.argv) > 2 else config.section("summarisation")["tiers"]["default"])
    prompt = (config.path("prompts") / "label.md").read_text(encoding="utf-8")
    here = config.path("perf_loop")
    with open(here / f"labels-{tag}.txt", "w", encoding="utf-8") as out:
        for path in sorted((here / "transcripts").glob(f"{tag}-*.json")):
            note = json.loads(path.read_text(encoding="utf-8")).get("note", "")
            if not note:
                continue
            title, _ = model.generate(prompt + note + "\n", 16)
            line = f"{path.name.replace(f'{tag}-', '').replace('_mixed.json', ''):<28} | {title}"
            print(line, flush=True)
            out.write(line + "\n")


if __name__ == "__main__":
    main()
