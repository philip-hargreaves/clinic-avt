# Retrieval

Which embedder and ranking find the guideline recommendation that applies to a consultation,
and when should the Guidelines tab show nothing? Does that ranking carry over to a clinician's
own folder of PDFs?

## Data

The gold sets in `rag/gold/` and the guideline documents in `rag/sources/` (see
`rag/README.md`). The folder study reads the folder at `guidelines_folder` in the config.
Exports go to `rag_candidates` and results to `rag/results/`, each run with its `config.json`.
Needs the `evaluation[retrieval]` environment. Everything runs on the CPU, so the GPU stays free
for the note model.

## Selection study

```
python evaluation/retrieval/chunk.py nice                   # recommendation chunks
python evaluation/retrieval/gold.py build                   # query file from the gold sets
python evaluation/retrieval/export.py --role embedder       # and --role reranker
python evaluation/retrieval/embed.py <id> --precision int8
python evaluation/retrieval/evaluate.py <id> --precision int8
python evaluation/retrieval/report.py embedders <run>       # also rerankers, ordering
```

`evaluate.py` reports success@k, recall@k, nDCG@10, MRR and precision@1, and sweeps the
threshold on the negatives.

| Script | Does |
|---|---|
| `review.py` | Gold labels beside what a run retrieved |
| `latency.py` | Embedding and reranking time, and memory |
| `pdf_compare.py` | Two PDF text extractors compared |
| `vector_compare.py` | Exact search against three vector indexes |
| `native_check.py` | The C++ pipelines in `native/` against the Python harness. Cosine 0.999 or better passes. |

## Folder study

The shipped ranking was settled on the clinician's own folder.

```
python evaluation/retrieval/folder_study.py units    # the folder's PDFs through the engine's chunker
python evaluation/retrieval/folder_study.py embed
python evaluation/retrieval/folder_study.py check    # refuses gold that gives away its answer
python evaluation/retrieval/folder_study.py run      # every ranking variant
python evaluation/retrieval/folder_study.py pool     # unjudged results, blinded, for the judge
python evaluation/retrieval/folder_study.py score
python evaluation/retrieval/folder_eval.py --tag <tag> study    # every query through the engine
python evaluation/retrieval/folder_study.py score runs-engine-<tag>.jsonl
```

The splitter, filter, vote and floor in `folder_study.py` are ports of the engine's
`guidance_query.hpp` and `guidance_rank.hpp`, and `folder_study.py parity` checks them. Close the
app before running `folder_eval.py`, which starts the engine from the app's build folder.
