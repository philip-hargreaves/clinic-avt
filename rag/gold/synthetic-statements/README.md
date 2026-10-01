# Synthetic statements

Consultation extracts written for this project, each labelled with the NICE recommendations it
should retrieve. Weighted towards rheumatology, with general and paediatric rows. Written by
the project team with model assistance and reviewed.

`statements.jsonl` has one item per line.

| Field | Meaning |
|---|---|
| `qid` | `syn-NN` |
| `area` | rheumatology, general, paediatric or control |
| `kind` | statement, extract, negated, family-history or history |
| `condition`, `scenario` | What the item is about, and the situation it was written from |
| `text` | Note-style text |
| `expected_ids` | Recommendations to retrieve. Empty means none. |
| `expected_codes` | Guideline codes for the entity check |
| `rationale` | Why these recommendations |
| `status` | Review date |
