"""Judges blind what plain punctuation does to the sentences it changes.

    python plain_punctuation_check.py translate     both paths over the changed sentences
    python plain_punctuation_check.py tasks         two-way judge tasks per language
    python plain_punctuation_check.py score         MQM penalty and critical errors, before and after

Before: the BPE tokenizer on the raw sentence. After: the engine's path, punctuation made plain
and the SentencePiece tokenizer. Other sentences tokenize to identical ids, so only the changed
ones run. Decoding mirrors NllbTranslator.
"""

import json
import os
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import judge  # noqa: E402
from common.io import read_json, read_jsonl  # noqa: E402
from study import APP_MODELS, LANGUAGES, ROOT  # noqa: E402
from tokenizer_parity import PLAIN, english  # noqa: E402

MODEL = APP_MODELS / "nllb-200-600m-int8"
BEFORE = ROOT / "probe" / "tokenizers" / "bpe-shipped"
OUT = ROOT / "results" / "plain-punctuation.jsonl"


def translate():
    import openvino as ov
    import openvino_tokenizers  # noqa: F401  registers the tokenizer ops

    changed = sorted({t for t in english() if t.translate(PLAIN) != t})
    spec = read_json(MODEL / "languages.json")
    special, codes = spec["special"], spec["languages"]
    core = ov.Core()
    config = {"CACHE_DIR": str(ROOT / "probe" / "work" / "cache-plain"), "CACHE_MODE": "OPTIMIZE_SIZE"}

    def compile(path, properties=None):
        return core.compile_model(str(path), "CPU", properties or {}).create_infer_request()

    encoder = compile(MODEL / "openvino_encoder_model.xml", config)
    decoder = compile(MODEL / "openvino_decoder_model.xml", config)
    old_tok = compile(BEFORE / "openvino_tokenizer.xml")
    new_tok = compile(MODEL / "openvino_tokenizer.xml")
    detok = compile(MODEL / "openvino_detokenizer.xml")
    beam = ov.Tensor(np.array([0], dtype=np.int32))

    def ids_before(text):
        old_tok.infer([ov.Tensor(np.array([text]))])
        return old_tok.get_tensor("input_ids").data[0].tolist()

    def ids_after(text):
        new_tok.infer([ov.Tensor(np.array([text.translate(PLAIN)]))])
        return [special["sourceLang"]] + new_tok.get_tensor("input_ids").data[0].tolist() + [special["eos"]]

    def decode(ids, target):
        mask = np.ones((1, len(ids)), dtype=np.int64)
        encoder.set_tensor("input_ids", ov.Tensor(np.array([ids], dtype=np.int64)))
        encoder.set_tensor("attention_mask", ov.Tensor(mask))
        encoder.infer()
        hidden = encoder.get_output_tensor(0)
        decoder.reset_state()
        step, out = [special["decoderStart"], target], []
        while len(out) < 512:
            decoder.set_tensor("input_ids", ov.Tensor(np.array([step], dtype=np.int64)))
            decoder.set_tensor("encoder_hidden_states", hidden)
            decoder.set_tensor("encoder_attention_mask", ov.Tensor(mask))
            decoder.set_tensor("beam_idx", beam)
            decoder.infer()
            last = decoder.get_tensor("logits").data[0, -1]
            token = int(last.argmax())
            if len(out) >= 2 and token == out[-1] == out[-2]:
                copy = last.copy()
                copy[token] = -1e9
                token = int(copy.argmax())
            if token == special["eos"]:
                break
            out.append(token)
            step = [token]
        detok.infer([ov.Tensor(np.array([out], dtype=np.int64))])
        return detok.get_output_tensor().str_data[0].replace("\u2581", " ").lstrip()

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        for language in LANGUAGES:
            target = codes[language]["id"]
            for text in changed:
                row = {"language": language, "source": text,
                       "before": decode(ids_before(text), target), "after": decode(ids_after(text), target)}
                f.write(json.dumps(row, ensure_ascii=False) + "\n")
            print(language, "done", flush=True)
    rows = read_jsonl(OUT)
    print(len(changed), "sentences,", len(rows), "translations,",
          sum(r["before"] != r["after"] for r in rows), "differ")


def tasks():
    by_language = defaultdict(list)
    for row in read_jsonl(OUT):
        if row["before"] != row["after"]:
            by_language[row["language"]].append(row)
    for language, rows in by_language.items():
        items = [(r["source"], [("before", r["before"]), ("after", r["after"])]) for r in rows]
        # Ten items per task
        for part in range(0, len(items), 10):
            judge.write_task("plain", f"plain__{language}__{part // 10}", language, items[part:part + 10])
    print({k: len(v) for k, v in by_language.items()})


def score():
    totals = defaultdict(lambda: {"penalty": 0.0, "critical": 0, "items": 0, "better": 0})
    for _, key, verdict in judge.verdicts("plain"):
        for number, item in key["items"].items():
            penalty = {}
            for letter, system in item["letters"].items():
                errors = verdict["items"].get(number, {}).get(letter, {}).get("errors", [])
                penalty[system] = judge.penalty(errors, item["words"])
                totals[system]["penalty"] += penalty[system]
                totals[system]["critical"] += sum(e.get("severity") == "critical" for e in errors)
                totals[system]["items"] += 1
            if penalty["after"] < penalty["before"]:
                totals["after"]["better"] += 1
            elif penalty["before"] < penalty["after"]:
                totals["before"]["better"] += 1
    for system, t in totals.items():
        print(f"{system:<7} items {t['items']}  mean MQM/100w {t['penalty'] / max(t['items'], 1):.1f}  "
              f"critical {t['critical']}  judged better on {t['better']}")


if __name__ == "__main__":
    {"translate": translate, "tasks": tasks, "score": score}[sys.argv[1]]()
