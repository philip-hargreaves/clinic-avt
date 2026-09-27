# Runs the label prompt over the 57 sweep notes on the resident-note model
# so every title can be read before the prompt ships. One GPU job.
import glob
import json
import os
import sys

import openvino_genai as og

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MODEL = os.path.join(ROOT, "models", "qwen3.5-9b-int4")
PROMPT = open(os.path.join(ROOT, "prompts", "label.md"), encoding="utf-8").read()
# Reads the transcripts perf_loop.py saves with PERF_SAVE and writes the titles beside them
HERE = os.environ.get("PERF_DIR", os.path.join(ROOT, "build", "perf-loop"))


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "sweep20"
    pipe = og.LLMPipeline(MODEL, "GPU", CACHE_DIR=os.path.join(MODEL, ".cache"))
    config = og.GenerationConfig()
    config.max_new_tokens = 16
    config.do_sample = False
    config.apply_chat_template = False
    out = open(os.path.join(HERE, f"labels-{tag}.txt"), "w", encoding="utf-8")
    for path in sorted(glob.glob(os.path.join(HERE, "transcripts", f"{tag}-*.json"))):
        note = json.load(open(path, encoding="utf-8")).get("note", "")
        if not note:
            continue
        wrapped = ("<|im_start|>user\n" + PROMPT + note +
                   "\n<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n")
        title = pipe.generate(wrapped, config).strip()
        name = os.path.basename(path).replace(f"{tag}-", "").replace("_mixed.json", "")
        line = f"{name:<28} | {title}"
        print(line, flush=True)
        out.write(line + "\n")
    out.close()


if __name__ == "__main__":
    main()
