# Summarisation

Which model and prompt write the most faithful and complete clinical note, and how do the
shipped tiers do on the app's own transcripts?

## Data

PriMock57 transcripts, with the 20 consultation checklists and labelled candidate notes that
come with the dataset. The app's transcripts come from a performance sweep, named by
`[summarisation] transcript_tag` in the config. MTS-Dialog is used for BERTScore. Working data
goes to `build/evaluation/summarisation/`.

## Run

```
python evaluation/summarisation/prep.py
python evaluation/summarisation/generate.py study --model <model>    # study prompt, reference transcripts
python evaluation/summarisation/generate.py app --tier default       # app prompts, app transcripts
python evaluation/summarisation/judge.py tasks <tag>                 # one task file per note
python evaluation/summarisation/judge.py collect <tag> --judge <judge model>
python evaluation/summarisation/score.py notes <tag>
```

| Script | Reports |
|---|---|
| `rigor.py` | Judge repeatability, and agreement with the clinicians' labels |
| `essential.py` | Coverage of the essential checklist items |
| `metrics.py` | Length limits, headings, readability |
| `leaks.py` | Prompt examples copied into notes |
| `sheet_checklist.py` | Checklists for judging the patient sheet |
| `case_summary.py` | Anonymisation of the appraisal summary |
| `bertscore.py` | BERTScore on MTS-Dialog |
| `label_bench.py`, `label_e2e.py` | Consultation titles |

## Judge

| Score | Definition |
|---|---|
| Faithfulness | 1 − fabricated claims / claims, over all 57 consultations |
| Completeness | Checklist items present / items, over the 20 with a checklist |

Each note is judged by a fresh Claude Opus session that sees one task file: the judge prompt,
the transcript the note was written from, the note and the checklist. It answers in
`judge-schema.json`.

`judge-prompt.md`, `judge-schema.json` and the study prompts in `prompts/` are frozen. The
app's own prompts are in `prompts/` at the repo root. Decoding is greedy throughout.
