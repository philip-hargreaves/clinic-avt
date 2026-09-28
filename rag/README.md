# rag

Guidance retrieval data. Private by default; a folder is committed only when named in `.gitignore`.

```
sources/nice/          fetched NICE corpus: manifest, html, json, pdfs, scripts, logs
sources/st-georges/    client guideline folder and referral cases
gold/                  labelled evaluation sets
candidates/            embedder and reranker exports under trial
results/               selection evaluation outputs
corpora/<id>/          built corpora: manifest.json, corpus.db; junctioned beside the exe as corpora
venv/                  harness environment, see evaluation/retrieval
```

Rules

- the engine reads a corpus only through `corpora/<id>/manifest.json`; nothing under `sources/` is indexed at runtime
- corpus ids carry the fetch date
- built corpora are not committed, and none ships through `weights/` yet
- committed here: this file, and folders named in `.gitignore`; third-party material carries `LICENCE` and `ATTRIBUTION`
- staged models live in `models/`, the harness in `evaluation/retrieval`, PR bodies in `internal/`
