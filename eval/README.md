# Evaluation suite

Everything that measures ClinicAVT, one folder per pipeline stage. Each stage README states its
question, where its data comes from, how to run it, what it writes and which result it backs.
The results themselves are written up in the research docs (`docs/evaluation/` in the
intelliscribe repository) and the dissertation.

| Stage | Question | Data | Main command | Headline result | Doc |
|---|---|---|---|---|---|
| [asr](asr/) | Which Whisper export transcribes best, at what cost? | PriMock57 | `asr/score.py` | turbo INT8 default; INT8 free, INT4 about +0.8 pt WER | 2-transcription/transcription-selection |
| [diarisation](diarisation/) | Right speaker for each word, doctor named without a wrong guess? | PriMock57, engine sweeps | `diarisation/runner_accuracy.py`, `score_gate.py` | engine reproduces the 96.9% research pipeline | 3-diarisation/ |
| [summarisation](summarisation/) | Most faithful, complete note; best prompt; shipped tiers on real transcripts | PriMock57 + checklists | `summarisation/score.py` | Qwen tiers 4B / 9B / 35B-A3B with the safety prompt | 5-summarisation/summariser-selection |
| [retrieval](retrieval/) | Right NICE recommendation, and silence when none applies | NICE corpus, gold sets | `retrieval/report.py` | gte-large INT8 shipped, with an abstention floor | 7-retrieval/retrieval-selection |
| [translation](translation/) | Most faithful sheet translation in eight languages, and its cost | FLORES-200, TICO-19, sheets | `translation/score.py`, `judge.py` | NLLB-200 600M INT8 | 4-translation/translation-selection |
| [performance](performance/) | Stop-to-note time, stability overnight, decode rate and memory | demo tracks, PriMock | `performance/perf_loop.py`, `latency.py` | stop-to-last-token per tier at 1x | 6-performance/performance-report |
| [knowledge](knowledge/) | How much medicine does each note model know? | MedQA, MedMCQA, PubMedQA, MMLU | `knowledge/run.py` | Qwen3.6-27B 84.4, Qwen3.5-4B 73.8 | 1-knowledge/knowledge-benchmarks |

## Layout

```
eval/
  config.toml        every dataset, model and output path, and the model registry
  pyproject.toml     dependencies, one extra per stage
  common/            config loader, engine pipe client, PriMock parsing, note-model loader, io, bootstrap
  <stage>/           scripts, README, frozen result-defining files (prompts, judge prompt, references)
```

Scripts run from the repo root, e.g. `python eval/asr/score.py`. Paths come from
`eval/config.toml`; any path can be overridden with `EVAL_<NAME>` (for example
`EVAL_PRIMOCK57=E:\primock57`). Everything a run writes goes under `build/eval/` or the stage's
working folder, never into the repo. Adding a note model is a `[models."name"]` entry in the
config: its path, `pipeline` (llm or vlm), `template` (`chatml`, the engine's own wrap, or
`model`, its chat template with thinking off) and plugin properties.

## Environment

Python 3.12 on Windows. Install the stage extras into a venv:

```
py -3.12 -m venv build\eval\venv
build\eval\venv\Scripts\python -m pip install -e "eval[asr,summarisation,performance]"
```

Three groups pin different transformers versions and need their own venvs: `knowledge`
(transformers 4, the backend the banked results used), `retrieval` and `translation`
(transformers 5, OpenVINO 2026.3), and `comet` (unbabel-comet, transformers 4, numpy 1).
torch is the CPU build (`--extra-index-url https://download.pytorch.org/whl/cpu`). Downloads on
this network need IPv4; the scripts force it (`EVAL_IPV6=1` to stop).

## Rules

- **One GPU job at a time.** Model runs, the engine, the app and its test suite all share the
  iGPU. Two at once corrupt driver state and both look like regressions. Close the app first.
- GPU only for note models; a load failure raises, there is no CPU fallback.
- Accept a timing only from a 1x run with the machine untouched and the lid open.
- Judges run isolated: one fresh agent per task file, nothing else in its context.

## Data

Nothing here is committed: datasets and outputs stay out of git (`.gitignore`).

| Dataset | Get it | Config key |
|---|---|---|
| PriMock57 | `git clone https://github.com/babylonhealth/primock57` | `primock57` |
| mixed PriMock tracks | `python eval/asr/mix.py` | `mixed_audio` |
| MTS-Dialog validation | `https://github.com/abachaa/MTS-Dialog` (the 100-pair validation file) | `mts_dialog` |
| FLORES-200, TICO-19 | `https://github.com/openlanguagedata/flores`, `https://tico-19.github.io` into `<mt_root>/data` | `mt_root` |
| knowledge tasks | fetched by lm-evaluation-harness on first run | — |
| NICE corpus | `rag/README.md` | `rag` |
| comparison models | `D:\models-eval\openvino\README.md` (`fetch_pinned.py`) | `comparison_models` |
| app models | `tools/ClinicAVT.FetchModels` | `app_models` |
