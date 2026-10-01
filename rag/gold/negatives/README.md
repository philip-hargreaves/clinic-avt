# Negatives

Inputs that should retrieve nothing, used for the false-positive rate and the threshold.
Written for this project.

| Kind | Examples |
|---|---|
| Non-clinical text | A CV, meeting notes, a recipe, a changelog |
| Clinical notes with no guideline in the corpus | Tennis elbow, motion sickness, chilblains |

`negatives.jsonl` has one item per line, with `qid`, `kind`, `mode` and `text`.
