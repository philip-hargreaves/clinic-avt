"""Compares a candidate tokenizer with the reference: first-call time, ids and unknown tokens.

    python tokenizer_parity.py <reference dir> <candidate dir>

The candidate runs as the engine runs it, with punctuation made plain and the source language
and end tokens added. The sentences are the patient sheets' and the FLORES-200 devtest.
"""

import json
import os
import sys
import time
from pathlib import Path

import numpy as np
import openvino as ov
import openvino_tokenizers  # noqa: F401  registers the tokenizer ops

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import read_json  # noqa: E402
from study import ROOT, SHEETS  # noqa: E402

FLORES = ROOT / "data" / "flores200_dataset" / "devtest" / "eng_Latn.devtest"
PLAIN = str.maketrans({"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2013": "-",
                       "\u2014": "-", "\u00a0": " ", "\u2026": "..."})
UNK = 3


def english() -> list[str]:
    from translate import sentences
    texts = [s for p in sorted(SHEETS.glob("*.md")) for line in p.read_text(encoding="utf-8").split("\n")
             for s in sentences(line)]
    return texts + FLORES.read_text(encoding="utf-8").splitlines()


def main():
    reference, candidate = Path(sys.argv[1]), Path(sys.argv[2])
    special = read_json(reference / "languages.json")["special"] \
        if (reference / "languages.json").exists() else {"sourceLang": 256047, "eos": 2}
    core = ov.Core()
    requests, first = {}, {}
    for name, folder in (("reference", reference), ("candidate", candidate)):
        t = time.perf_counter()
        model = core.compile_model(str(folder / "openvino_tokenizer.xml"), "CPU")
        requests[name] = model.create_infer_request()
        requests[name].infer([ov.Tensor(np.array(["Ready."]))])
        first[name] = round(time.perf_counter() - t, 3)

    def ids(name, text):
        requests[name].infer([ov.Tensor(np.array([text]))])
        return requests[name].get_tensor("input_ids").data[0].tolist()

    texts = english()
    mismatches, unknown = [], {"reference": 0, "candidate": 0}
    for text in texts:
        a = ids("reference", text)
        b = [special["sourceLang"]] + ids("candidate", text.translate(PLAIN)) + [special["eos"]]
        unknown["reference"] += a.count(UNK)
        unknown["candidate"] += b.count(UNK)
        if a != b and a.count(UNK) == 0:
            mismatches.append((text, a, b))
    changed = sum(t.translate(PLAIN) != t for t in texts)
    print(json.dumps({"first_call_s": first, "sentences": len(texts), "made_plain": changed,
                      "mismatches_where_reference_has_no_unknown": len(mismatches),
                      "examples": mismatches[:3], "unknown_tokens": unknown}, ensure_ascii=False))


if __name__ == "__main__":
    main()
