"""Compares the app's NLLB IR with the study's fresh export.

    python parity.py [--sheets 6]

Both IRs run through the same harness on the same sheets in every language. Reports the share of
identical sentences and the chrF++ of one against the other.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from study import APP_MODELS, LANGUAGES, ROOT, option  # noqa: E402

SHIPPED = APP_MODELS / "nllb-200-600m-int8"


def main():
    from sacrebleu.metrics import CHRF
    from translate import Translator, load_set, sentences
    count = int(option("--sheets", "6"))
    sheets = [s for s in load_set("sheets", count) if s["language"] == "Urdu"]
    fresh = Translator("nllb-600m", "int8", 8)
    shipped = Translator("nllb-600m", "int8", 8)
    from optimum.intel import OVModelForSeq2SeqLM
    shipped.model = OVModelForSeq2SeqLM.from_pretrained(
        SHIPPED, device="CPU", ov_config={"INFERENCE_NUM_THREADS": "8"})
    shipped.model.generation_config.max_length = None
    same = total = 0
    a_all, b_all = [], []
    for language in LANGUAGES:
        for sheet in sheets:
            a = sentences(fresh.document(sheet["source"], language).replace("\n", " "))
            b = sentences(shipped.document(sheet["source"], language).replace("\n", " "))
            total += max(len(a), len(b))
            same += sum(x == y for x, y in zip(a, b))
            a_all.append(" ".join(a))
            b_all.append(" ".join(b))
        print(language, "done", flush=True)
    chrf = CHRF(word_order=2).corpus_score(b_all, [a_all]).score
    result = {"sheets": count, "languages": len(LANGUAGES), "sentences": total,
              "identical_share": round(same / total, 4), "chrf_shipped_vs_fresh": round(chrf, 2)}
    print(json.dumps(result))
    json.dump(result, open(ROOT / "results" / "parity.json", "w", encoding="utf-8"), indent=1)


if __name__ == "__main__":
    main()
