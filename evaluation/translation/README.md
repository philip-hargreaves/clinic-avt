# Translation

Which on-device model translates the patient sheet most faithfully into the eight languages the
practice needs, and what does the translator cost to start, run and hold in memory?

## Data

FLORES-200 devtest, TICO-19, the accuracy tier's patient sheets
(`evaluation/summarisation/generate.py app --tier accuracy`) and the candidates in
`candidates.json`. Models, test sets and results live under `mt_root` in the config.

The scripts need the `evaluation[translation]` environment. `comet_qe.py` and
`tokenizer_convert.py` need `evaluation[comet]`. Install both with
`--extra-index-url https://download.pytorch.org/whl/cpu`. `score.py refs` needs `SACREBLEU` set
to a folder holding the FLORES-200 SentencePiece model.

## Selection study

| Script | Does |
|---|---|
| `export.py <id>` | Exports a candidate to OpenVINO |
| `translate.py <id> <set>` | Translates `flores`, `tico` or `sheets` |
| `qwen_translate.py` | Translates with the note model as an LLM reference, on the GPU with the app closed |
| `score.py` | chrF++ and spBLEU against references, integrity checks on the sheets |
| `judge.py` | Blind judging of the sheets, validated on planted errors |
| `comet_qe.py` | Reference-free COMET |
| `cost.py` | Cold start, seconds per sheet, memory |
| `parity.py` | The app's model against the study's export |

## Translator lifecycle

| Script | Does |
|---|---|
| `tokenizer_convert.py` | Builds the tokenizer the app ships |
| `tokenizer_parity.py` | Compares a tokenizer with the reference |
| `plain_punctuation_check.py` | Blind check of the sentences that plain punctuation changes |
| `lifecycle_probe.py` | Cold start, speed and memory per CPU setting |
| `lifecycle_smoke.py` | The lifecycle in a real engine, over the pipe |

Each judge verdict comes from an isolated session that reads only `judge-prompt.md` and one
task file. Each script's docstring gives its arguments.
