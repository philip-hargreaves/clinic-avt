# transcription

**Question.** Which Whisper export transcribes UK primary-care consultations best on this laptop,
at what speed and memory, and what does quantisation cost?

**Data.** PriMock57 (57 mock GP consultations, per-speaker WAVs and Praat TextGrids):
`git clone https://github.com/babylonhealth/primock57` into `primock57` in `evaluation/config.toml`.
`mix.py` builds the single-microphone tracks the app sees. The export suite goes to `asr_models`.

**Backs.** Research docs `docs/evaluation/2-transcription/` (transcription-selection,
scoring-methodology, model-suite): turbo INT8 default, large-v3 INT8 accuracy, turbo INT4 constrained.

## Run

```
python evaluation/transcription/mix.py                          # <cid>_mixed.wav into mixed_audio
python evaluation/transcription/references.py                   # build/evaluation/transcription/references/<cid>.json
python evaluation/transcription/transcribe.py [--models a,b] [--limit N] [--forms mixed,doctor,patient]
python evaluation/transcription/score.py [--form mixed] [--clinical] [--hyp <folder>]
```

Outputs under `build/evaluation/transcription/`: `hyp/<export>/<cid>_<form>.json` (text, audio and processing
seconds, RTFx) with `_summary.json` (load, peak memory), `scores/<export>__<form>.jsonl` per input
and `scores/aggregate.json`.

## Metrics

Reference and hypothesis go through the same normalisation (`normalise.py`: Whisper's English
normaliser, vendored under MIT, plus UK-to-US clinical spellings). WER and CER from one
Levenshtein alignment; content WER discounts filler backchannels unless they answer a question.
`--clinical` adds concept, drug and dose recall and negation kept, from scispaCy
`en_ner_bc5cdr_md` (install line in `evaluation/pyproject.toml`).

## Export

The suite (17 exports) was built with optimum-intel from the Hugging Face checkpoints, e.g.

```
optimum-cli export openvino -m openai/whisper-large-v3-turbo --weight-format int8 <asr_models>/whisper-large-v3-turbo-int8-ov
```

with `int4` and `fp16` likewise; the NPU variants and every flag are in the research doc
`docs/evaluation/2-transcription/model-suite.md`. The list is
`[transcription] models` in the config. The shipped model is `whisper-turbo-int8` in `models/`.
