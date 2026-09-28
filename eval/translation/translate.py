"""Translates a test set with one exported candidate on the CPU.

    python translate.py <id> <set> [--weights int8] [--limit N] [--threads 8]

Sets: flores (FLORES-200 devtest), tico (TICO-19 test), sheets (the app's patient sheets).
Every model gets the engine's line and sentence splitting, one sentence per input, greedy
decoding and the same thread cap. Writes results/<set>/<id>-<weights>.jsonl, one row
per item and language, and skips rows already there.
"""

import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import read_jsonl  # noqa: E402
from study import LANGUAGES, ROOT, SHEETS, option  # noqa: E402

CANDIDATES = json.load(open(Path(__file__).with_name("candidates.json"), encoding="utf-8"))
FLORES = {"Urdu": "urd_Arab", "Punjabi": "pan_Guru", "Bengali": "ben_Beng", "Gujarati": "guj_Gujr",
          "Polish": "pol_Latn", "Romanian": "ron_Latn", "Arabic": "arb_Arab", "Somali": "som_Latn"}
TICO = {"Urdu": "ur", "Bengali": "bn", "Arabic": "ar", "Somali": "so"}
MAX_NEW_TOKENS = 256
BATCH = 8


def sentences(line: str) -> list[str]:
    """The engine's split: a sentence ends at '. ', '? ' or '! '."""
    out, start = [], 0
    while start < len(line):
        end = len(line)
        for mark in (". ", "? ", "! "):
            at = line.find(mark, start)
            if at != -1 and at + 1 < end:
                end = at + 1
        piece = line[start:end]
        if piece.strip():
            out.append(piece.strip())
        start = end + (1 if end < len(line) else 0)
    return out


def load_set(name: str, limit: int) -> list[dict]:
    """Items as {id, language, source, reference}. A sheet has no reference."""
    items = []
    if name == "flores":
        root = ROOT / "data" / "flores200_dataset" / "devtest"
        english = open(root / "eng_Latn.devtest", encoding="utf-8").read().splitlines()[:limit]
        for language in LANGUAGES:
            target = open(root / f"{FLORES[language]}.devtest", encoding="utf-8").read().splitlines()
            items += [{"id": f"flores-{i:04d}", "language": language, "source": s, "reference": target[i]}
                      for i, s in enumerate(english)]
    elif name == "tico":
        for language, code in TICO.items():
            path = ROOT / "data" / "tico19" / "tico19-testset" / "test" / f"test.en-{code}.tsv"
            rows = open(path, encoding="utf-8").read().splitlines()[1:limit + 1]
            for row in rows:
                cells = row.split("\t")
                items.append({"id": "tico-" + cells[4], "language": language, "source": cells[2],
                              "reference": cells[3]})
    elif name == "sheets":
        for path in sorted(SHEETS.glob("*.md"))[:limit]:
            text = path.read_text(encoding="utf-8").strip()
            items += [{"id": path.stem, "language": language, "source": text, "reference": ""}
                      for language in LANGUAGES]
    return items


class Translator:
    def __init__(self, name: str, weights: str, threads: int):
        from optimum.intel import OVModelForSeq2SeqLM
        from transformers import AutoTokenizer

        self.entry = CANDIDATES[name]
        self.family = self.entry["family"]
        directory = ROOT / "models" / f"{name}-{weights}"
        started = time.time()
        config = {"INFERENCE_NUM_THREADS": str(threads)} if threads else {}
        self.model = OVModelForSeq2SeqLM.from_pretrained(directory, device="CPU", ov_config=config)
        self.tokenizer = AutoTokenizer.from_pretrained(directory)
        self.model.generation_config.max_length = None  # max_new_tokens is the only cap
        self.load_seconds = time.time() - started

    def batch(self, texts: list[str], language: str) -> list[str]:
        code = self.entry["codes"][language]
        tok, extra = self.tokenizer, {}
        if self.family == "nllb":
            tok.src_lang = self.entry["source"]
            extra["forced_bos_token_id"] = tok.convert_tokens_to_ids(code)
        elif self.family == "m2m100":
            tok.src_lang = self.entry["source"]
            extra["forced_bos_token_id"] = tok.get_lang_id(code)
        elif self.family == "small100":
            # SMALL-100 reads the target language from the front of the source
            tok.src_lang = code
        elif self.family == "madlad":
            texts = [f"<2{code}> {t}" for t in texts]
        inputs = tok(texts, return_tensors="pt", padding=True, truncation=True, max_length=512)
        output = self.model.generate(**inputs, num_beams=1, do_sample=False,
                                     max_new_tokens=MAX_NEW_TOKENS, **extra)
        return [t.strip() for t in tok.batch_decode(output, skip_special_tokens=True)]

    def document(self, text: str, language: str) -> str:
        """Line by line and sentence by sentence, as the engine does, so structure survives."""
        lines = text.split("\n")
        pieces = [(i, s) for i, line in enumerate(lines) for s in sentences(line)]
        translated = []
        for k in range(0, len(pieces), BATCH):
            translated += self.batch([s for _, s in pieces[k:k + BATCH]], language)
        out = [""] * len(lines)
        for (i, _), t in zip(pieces, translated):
            out[i] = (out[i] + " " + t).strip()
        return "\n".join(out)


def main():
    name, set_name = sys.argv[1], sys.argv[2]
    weights = option("--weights", "int8")
    limit, threads = int(option("--limit", "100000")), int(option("--threads", "8"))
    out = ROOT / "results" / set_name / f"{name}-{weights}.jsonl"
    out.parent.mkdir(parents=True, exist_ok=True)
    done = set()
    if out.exists():
        done = {(r["id"], r["language"]) for r in read_jsonl(out)}
    items = [i for i in load_set(set_name, limit) if (i["id"], i["language"]) not in done]
    print(name, set_name, len(items), "to do,", len(done), "done", flush=True)
    if not items:
        return
    # One writer per output file
    lock = out.with_suffix(".lock")
    try:
        lock.touch(exist_ok=False)
    except FileExistsError:
        print("another run holds", lock.name, flush=True)
        return
    try:
        work(name, set_name, weights, threads, items, out)
    finally:
        lock.unlink(missing_ok=True)


def work(name, set_name, weights, threads, items, out):
    translator = Translator(name, weights, threads)
    print("loaded in %.1f s" % translator.load_seconds, flush=True)
    with open(out, "a", encoding="utf-8", newline="\n") as f:
        if set_name == "sheets":
            for n, item in enumerate(items):
                started = time.time()
                text = translator.document(item["source"], item["language"])
                f.write(json.dumps({**item, "system": name, "translation": text,
                                    "seconds": round(time.time() - started, 2)},
                                   ensure_ascii=False) + "\n")
                f.flush()
                if n % 40 == 0:
                    print(n, "/", len(items), flush=True)
        else:
            for language in LANGUAGES:
                rows = [i for i in items if i["language"] == language]
                for k in range(0, len(rows), BATCH):
                    chunk = rows[k:k + BATCH]
                    started = time.time()
                    texts = translator.batch([i["source"] for i in chunk], language)
                    seconds = round((time.time() - started) / len(chunk), 3)
                    for item, text in zip(chunk, texts):
                        row = {**item, "system": name, "translation": text, "seconds": seconds}
                        f.write(json.dumps(row, ensure_ascii=False) + "\n")
                    f.flush()
                print(language, len(rows), flush=True)


if __name__ == "__main__":
    main()
