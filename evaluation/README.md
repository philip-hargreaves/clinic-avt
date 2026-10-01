# Evaluation

The scripts that measure ClinicAVT, one folder per pipeline stage. Each stage's README gives
its question, data and commands.

| Stage | Question | Data | Main script |
|---|---|---|---|
| [knowledge](knowledge/) | How much medicine does each candidate note model know? | MedQA, MedMCQA, PubMedQA, MMLU | `run.py` |
| [transcription](transcription/) | Which Whisper export transcribes best, and at what cost? | PriMock57 | `score.py` |
| [diarisation](diarisation/) | Is each word given to the right speaker? | PriMock57 | `runner_accuracy.py` |
| [summarisation](summarisation/) | How faithful and complete are the notes? | PriMock57 and its checklists | `judge.py`, `score.py` |
| [translation](translation/) | How faithful is the translated patient sheet? | FLORES-200, TICO-19 | `score.py`, `judge.py` |
| [retrieval](retrieval/) | Does the search find the right guideline, and nothing when none applies? | Guideline corpus, `rag/gold` | `report.py` |
| [performance](performance/) | How long from stop to note, and how much memory? | Example recordings, PriMock57 | `perf_loop.py` |

## Layout

```
evaluation/
├── config.toml       dataset, model and output paths
├── pyproject.toml    dependencies, one extra per stage
├── common/           config loader, engine client, statistics
└── <stage>/          scripts and the files that define a result, such as prompts
```

## Running

Scripts run from the repo root with Python 3.12.

```
py -3.12 -m venv build\evaluation\venv
build\evaluation\venv\Scripts\python -m pip install -e "evaluation[transcription,summarisation,performance]"
python evaluation/transcription/score.py
```

Paths come from `config.toml`, and `EVAL_<NAME>` overrides one, for example
`EVAL_PRIMOCK57=E:\primock57`. Outputs go under `build/evaluation/`. The `knowledge`, `retrieval`
with `translation`, and `comet` extras pin different transformers versions, so each needs its
own environment.

Run one GPU job at a time. The engine, the app and the model runs share the integrated GPU.

## Data

Datasets and outputs are not in git.

| Dataset | Source | Config key |
|---|---|---|
| PriMock57 | https://github.com/babylonhealth/primock57 | `primock57` |
| Mixed PriMock57 tracks | `python evaluation/transcription/mix.py` | `mixed_audio` |
| MTS-Dialog | https://github.com/abachaa/MTS-Dialog | `mts_dialog` |
| FLORES-200, TICO-19 | https://github.com/openlanguagedata/flores, https://tico-19.github.io | `mt_root` |
| Knowledge tasks | Fetched by lm-evaluation-harness on first run | |
| App models | `tools/ClinicAVT.FetchModels` | `app_models` |
