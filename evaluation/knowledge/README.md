# Knowledge

How much medicine does each candidate note model know before it writes a note? This is a
screening step. The note evaluation makes the selection.

## Data

MedQA, MedMCQA, PubMedQA and six MMLU medical subjects, downloaded by lm-evaluation-harness on
first use. The task list is `[knowledge] tasks` in `evaluation/config.toml`.

## Run

```
python evaluation/knowledge/run.py <model>              # full suite
python evaluation/knowledge/run.py <model> --limit 5    # five questions per task
python evaluation/knowledge/run.py --table              # averages and per-task scores
```

Results go to `build/evaluation/knowledge/results/<model>.json`. Scoring is zero-shot by
log-likelihood on the GPU. Gemma models need `bos = true` in the config. This stage needs its
own environment, `evaluation[knowledge]`.
