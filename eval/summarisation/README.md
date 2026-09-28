# summarisation

**Question.** Which on-device model writes the most faithful and complete clinical note, under
which prompt, and how do the shipped tiers do on the transcripts the app itself produces?

**Data.** PriMock57 (see `eval/asr/README.md`): the TextGrids, and the 20 Clinician_1 consultation
checklists with the dataset's labelled candidate notes. The app's sealed transcripts come from a
perf-loop sweep (`[summarisation] transcript_tag`). MTS-Dialog validation (100 pairs) for BERTScore.
Working data goes to `summarisation` in `eval/config.toml` (default `build/eval/summarisation`):
`prep/`, `notes/`, `judge/`, `runs/`, `reps/`.

**Backs.** Research docs `docs/evaluation/5-summarisation/` (screening-results, prompt-tuning,
summariser-selection, note-tiers-baseline-2026-09, rigor-pilot, bertscore, case-summary-2026-09).

## Run

```
python eval/summarisation/prep.py
python eval/summarisation/generate.py study --model qwen3.5-4b-int4-ov [--prompt prompt-4b-safety.md --tag qwen3.5-4b-safety]
python eval/summarisation/generate.py app --tier default                       # app prompts, sealed transcripts
python eval/summarisation/generate.py app --model gemma-4-31b-it-int4-ov --limit 2 --details standard
python eval/summarisation/judge.py tasks <tag> [--transcripts sealed]
python eval/summarisation/judge.py pending <tag>
python eval/summarisation/judge.py collect <tag> --judge <judge model>
python eval/summarisation/score.py notes <tag>...    |  screening  |  bootstrap
python eval/summarisation/rigor.py reliability | validity | severity
python eval/summarisation/metrics.py [--tags tier-default,<model>]   deterministic: limits, headings, readability
python eval/summarisation/leaks.py [tag...]                          prompt-example leaks
python eval/summarisation/case_summary.py [--tier default] | --scan  appraisal summary anonymisation
python eval/summarisation/bertscore.py generate <model> | score [--dir]
python eval/summarisation/label_bench.py <sweep tag> [model]         |  label_e2e.py
```

## The judge

Faithfulness = 1 - fabrications / claims, over all 57. Completeness = checklist items present /
items, over the 20 with a checklist. Severity is reported, not gated. Each note is judged by a
fresh Claude Opus agent that sees only one task file: the frozen `judge-prompt.md`, the
transcript the note was written from, the note, and the checklist; it answers in
`judge-schema.json`. Validity: r 0.67 against the clinicians' own labels; reliability:
within-note SD 0.006 across passes.

## Frozen files

`judge-prompt.md`, `judge-schema.json`, `prompts/` (screening `prompt-v1.md` and the
prompt-tuning arms), `screening-context.json` (the knowledge and speed columns of the screening
table). The app's own prompts are the repo's `prompts/`.

Models come from `eval/config.toml`; `template = "chatml"` is the engine's own wrap, `"model"`
the model's chat template with thinking off. Greedy decoding throughout.
