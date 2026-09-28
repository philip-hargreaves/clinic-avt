# knowledge

**Question.** How much medicine does each candidate note model know, before it is asked to write a
note? A screening filter, not the selection criterion.

**Data.** Nine public multiple-choice tasks, downloaded by lm-evaluation-harness on first use
(Hugging Face datasets): MedQA (4 options), MedMCQA, PubMedQA and six MMLU medical subjects,
listed in `[knowledge] tasks` in `evaluation/config.toml`.

**Backs.** Research docs `docs/evaluation/1-knowledge/` (knowledge-benchmarks, knowledge-report,
comparison-models-2026-09, gemma4-struggles).

## Run

```
python evaluation/knowledge/run.py qwen3.5-9b-int4-ov                 # full suite -> build/evaluation/knowledge/results/<model>.json
python evaluation/knowledge/run.py gemma-4-31b-it-int4-ov --limit 5   # smoke: 5 questions per task -> smoke/
python evaluation/knowledge/run.py --table [folder]                   # average, MMLU medical, per task
```

Zero-shot, loglikelihood scoring on the iGPU, the automatic batch sized under the 4 GB allocation
cap. Two backends with one scoring rule: lm-eval's OpenVINO model for LLM exports (every banked
result), and a split backend for VLM exports, which runs the text embeddings and the language
model on ov.Core and never loads the vision parts. Gemma needs BOS prepended (`bos = true`), or
scores fall below chance. Knowledge needs its own environment (`evaluation[knowledge]`, transformers 4).
