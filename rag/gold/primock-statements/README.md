# PriMock statements

Sentences from the app's clinical notes of PriMock57 consultations, each labelled with the
NICE recommendations it should retrieve. See `LICENCE` and `ATTRIBUTION`.

`statements.jsonl` has one statement per line.

| Field | Meaning |
|---|---|
| `qid` | `pm-NN` |
| `consult` | The PriMock57 recording the note came from |
| `text` | The sentence as the note wrote it |
| `expected_ids` | Recommendations to retrieve. Empty means none. |
| `expected_codes` | Guideline codes for the entity check |
| `rationale` | Why these recommendations |
| `status` | Review date |
