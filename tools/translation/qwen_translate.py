"""Translates the judged sheets with the app's note model, as an LLM reference row.

    python qwen_translate.py [--model qwen3.5-4b-int4] [--sheets 12]
    python qwen_translate.py --set flores [--limit 300]

Same harness as the seq2seq candidates: the engine's line and sentence split, one sentence per
call, greedy. The model runs on the GPU through the GenAI pipeline, as it does in the app. Writes
results/<set>/<model>.jsonl with seconds per item, resuming rows already there.
"""

import json
import random
import time

from common import APP_MODELS, ROOT, option, read_jsonl

SYSTEM = ("You are a professional medical translator. "
          "Translate the user's sentence from English into {language}. "
          "Reply with the translation only: no notes, no alternatives, no English.")


def main():
    from translate import load_set, sentences
    import openvino_genai as genai
    model, count = option("--model", "qwen3.5-4b-int4"), int(option("--sheets", "12"))
    set_name, limit = option("--set", "sheets"), int(option("--limit", "300"))
    out = ROOT / "results" / set_name / f"{model}.jsonl"
    done = set()
    if out.exists():
        done = {(r["id"], r["language"]) for r in read_jsonl(out)}
    if set_name == "sheets":
        # The same 12 sheets the judge saw
        all_ids = sorted({s["id"] for s in load_set("sheets", 100)})
        chosen = set(random.Random(7).sample(all_ids, count))
        items = [s for s in load_set("sheets", 100) if s["id"] in chosen]
    else:
        items = load_set(set_name, limit)
    items = [s for s in items if (s["id"], s["language"]) not in done]
    print(model, set_name, len(items), "to do", flush=True)

    started = time.time()
    pipe = genai.LLMPipeline(str(APP_MODELS / model), "GPU")
    config = genai.GenerationConfig()
    config.max_new_tokens = 256
    config.do_sample = False
    config.apply_chat_template = False
    print("loaded in %.1f s" % (time.time() - started), flush=True)

    def translate(sentence: str, language: str) -> str:
        # Wrapped as the engine wraps it. The empty think block stops the model reasoning
        prompt = ("<|im_start|>user\n" + SYSTEM.format(language=language) + "\n\n" + sentence +
                  "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n")
        text = pipe.generate(prompt, config)
        return text.strip().split("\n")[0].strip()

    with open(out, "a", encoding="utf-8", newline="\n") as f:
        for n, item in enumerate(items):
            lines = item["source"].split("\n")
            started = time.time()
            translated = []
            for line in lines:
                translated.append(" ".join(translate(s, item["language"]) for s in sentences(line)))
            f.write(json.dumps({**item, "system": model, "translation": "\n".join(translated),
                                "seconds": round(time.time() - started, 2)}, ensure_ascii=False) + "\n")
            f.flush()
            if set_name == "sheets" or n % 100 == 0:
                seconds = time.time() - started
                print(n + 1, "/", len(items), item["language"], "%.1f s" % seconds, flush=True)


if __name__ == "__main__":
    main()
