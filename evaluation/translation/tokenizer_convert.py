"""Converts the NLLB tokenizer to the SentencePiece OpenVINO tokenizer the engine ships.

    python tokenizer_convert.py <model dir> <out dir>

The native BPE op builds its 256k-merge table on the first call, 13 to 26 s. The SentencePiece
op loads in about 0.25 s. It emits the sentence's ids alone, and the engine adds the source
language and end tokens. Needs transformers 4. Transformers 5 ignores the SentencePiece flag.
"""

import json
import sys
from pathlib import Path


def main():
    import openvino as ov
    import openvino_tokenizers
    import transformers
    from openvino_tokenizers import convert_tokenizer
    from transformers import NllbTokenizer

    model, out = Path(sys.argv[1]), Path(sys.argv[2])
    tokenizer = NllbTokenizer.from_pretrained(model, src_lang="eng_Latn")
    converted = convert_tokenizer(tokenizer, use_sentencepiece_backend=True, add_special_tokens=False)
    ops = {op.get_type_name() for op in converted.get_ops()}
    if "SentencepieceTokenizer" not in ops:
        raise SystemExit(f"not a SentencePiece tokenizer: {sorted(ops)}")
    out.mkdir(parents=True, exist_ok=True)
    ov.save_model(converted, str(out / "openvino_tokenizer.xml"))
    json.dump({"openvino": ov.__version__, "openvino_tokenizers": openvino_tokenizers.__version__,
               "transformers": transformers.__version__, "source": str(model / "sentencepiece.bpe.model"),
               "use_sentencepiece_backend": True, "add_special_tokens": False},
              open(out / "tokenizer-provenance.json", "w"), indent=1)
    print("written", out)


if __name__ == "__main__":
    main()
