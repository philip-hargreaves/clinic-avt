# Retrieval data

Data for the guideline search evaluation. Only the gold sets are in git.

```
rag/
├── gold/                        labelled evaluation sets
│   ├── primock-statements/      sentences from the app's notes of PriMock57 consultations
│   ├── synthetic-statements/    consultation extracts written for the project
│   └── negatives/               inputs that should retrieve nothing
├── sources/                     guideline documents, not in git
└── corpora/                     built corpora, not in git
```

The harness is in `evaluation/retrieval`.
