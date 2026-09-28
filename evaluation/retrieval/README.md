# retrieval

**Question.** Which embedder and ordering find the NICE recommendation that applies to a
consultation, and when should the Guidelines tab stay silent? Then: does the shipped ranking
transfer to a clinician's own folder of PDFs?

**Data.** The NICE corpus and gold sets under `rag/` (sources fetched by `rag/README.md`; committed
gold in `rag/gold/`), the clinician's folder (`guidelines_folder` in `evaluation/config.toml`),
exports under `rag_candidates`.

**Backs.** Research docs `docs/evaluation/7-retrieval/` (retrieval-selection, gold-sets,
engine-gates-2026-09-12, folder-transfer-2026-09-21 and its addendum, abstention-review).

Selection evaluation harness for the Guidelines feature. Reads `rag/sources` and `rag/gold`, writes `rag/candidates` and `rag/results`. Code only; no data here.

## Setup

```
py -3.12 -m venv rag\venv
rag\venv\Scripts\python -m pip install -e "evaluation[retrieval]" --extra-index-url https://download.pytorch.org/whl/cpu
```

`rag\venv`, `rag\candidates` and `rag\results` may be junctions to another drive. Checkpoints download to `rag\candidates\.src\<org>--<model>` without symlinks, so an exFAT drive works. IPv4 is forced for downloads; `EVAL_IPV6=1` disables that.

## Run order

```
python chunk.py nice                                   NICE JSON -> rag/results/chunks/nice-<fetchdate>.jsonl
python gold.py map-ucl                                 draft mapping; hand-check, save as mapping.jsonl
python gold.py build                                   -> rag/results/queries/queries-<date>.jsonl; refuses synthetic rows that echo their recommendation
python gold.py build-notes                             -> rag/results/queries/notes-<date>.jsonl; whole notes per labelled PriMock consultation, four sources
python gold.py build-transcripts                       -> rag/results/queries/transcripts-<date>.jsonl; transcript, doctor turns, note plus doctor turns
python export.py --role embedder                       fp16 and int8 IRs -> rag/candidates/<id>-<precision>
python export.py --role reranker
python embed.py <id> --precision int8 --reference      -> rag/results/emb/<id>-int8-text
python evaluate.py <id> --precision int8 --rerankers gte-reranker-modernbert,minilm-l6 --hybrid off,on
python evaluate.py <id> --precision int8 --rerankers medcpt-cross --dump-union   also writes every query's 50 candidates with both scores
python evaluate.py <id> --precision int8 --queries <file> --union rrf            note-level file; sub-query lists merged by rank vote (max|rrf|zmax)
python review.py <run> primock|synthetic              -> rag/results/<set>-review.md, labels beside what the run retrieved
python report.py embedders|rerankers|ordering <run>   -> rag/results/report-*.md; round tables with paired wins and bootstrap CIs
python report.py second-stage|union-rules|sources ... -> paired tests on identical queries: second stage against cosine order, union rules, query sources
python latency.py --embedders <ids> --rerankers <ids>
python pdf_compare.py                                  31 client PDFs, pypdfium2 against pdftotext
python vector_compare.py --emb rag/results/emb/<run>/docs.npy
```

Every run writes `config.json` beside its outputs under `rag/results/<stamp>-<name>/`.

## Files

```
shortlist.json      candidates: id, hf, role, licence, architecture, pooling, dims, max_length, instructions, export task
harness.py          paths, candidates, run directories, timing, pdftotext (io in evaluation/common)
chunk.py            recommendation chunks from NICE JSON; heading chunks from PDF text
gold.py             UCL mapping draft; unified query file from the five gold sets; wording-overlap check
export.py           optimum-cli export with provenance.json (revision, hashes); an `onnx` field in the shortlist converts the repo's ONNX file instead
embed.py            document embeddings on CPU; faithfulness against sentence-transformers
evaluate.py         success@k, recall@k, nDCG@10, MRR, p@1, entity match; threshold sweep on negatives; --v1-baseline; --dump-union
review.py           review table for one gold set from a run's per_query.jsonl
report.py           round tables and paired tests (bootstrap CI, sign test) from run outputs; see its docstring for the six reports
rerank_core.py      cross-encoder on ov.Core with explicit pairs; the default rerank backend
latency.py          per sentence, per note, scan, rerank 30 and 50 pairs; RSS
native_check.py     C++ proof against the Python pipelines and the reference model
pdf_compare.py      word agreement, order, recommendation patterns, image-only detection
vector_compare.py   numpy exact against hnswlib, usearch, sqlite-vec
native/             proof.cpp: same embed and rerank through the GenAI C++ pipelines, for parity
folder_study.py     offline study over the clinician's own folder: units, embeddings, ranking variants, blind judging, scoring
folder_eval.py      the St George's cases, or every study query, through the engine the app ships
```

## Folder study

The shipped ranking was settled on the clinician's own folder, which has its own pair of scripts
and its own working directory under `build/retrieval`.

```
python folder_study.py units                             engine chunker over the folder's PDFs
python folder_study.py embed                             the shipped embedder over the units and the queries
python folder_study.py check                             refuses gold that gives away its answer
python folder_study.py run                               every ranking variant over every query
python folder_study.py pool                              cards not yet judged, blinded, for the judge
python folder_study.py score                             the variants against the judgments
python folder_eval.py --tag before                       the St George's cases through the engine
python folder_eval.py --tag frozen study                 every study query through the engine
python folder_study.py score runs-engine-frozen.jsonl    that engine run, judged the same way
```

`FOLDER_STUDY=<name>` moves the working directory, for the same study over a changed folder or
chunker. The splitter, the exclusion filter, the vote and the floor in `folder_study.py` are ports
of the engine's `guidance_query.hpp` and `guidance_rank.hpp`, and `folder_study.py parity <file>`
proves the port against an earlier note-mode `folder_eval.py` run; the script now sends typed
searches only. Close the app before any `folder_eval.py` run. Each script's docstring has the rest.

## Native proof

```
cmake -S evaluation\retrieval\native -B D:\ambient-rag\native-build -G "Visual Studio 18 2026" -A x64 ^
  -DOpenVINO_DIR=external\openvino\runtime\cmake -DOpenVINOGenAI_DIR=external\openvino\runtime\cmake
cmake --build D:\ambient-rag\native-build --config Release
proof embed  <model_dir> cls 512 texts.txt
proof rerank <model_dir> 512 "<query>" texts.txt
```

Compare the printed vectors with `embed.py` output; cosine 0.999 or better is the gate. Run from a shell with `external\openvino\setupvars.bat` applied so the DLLs resolve.

## Notes

- CPU only. The GPU stays free for the note model.
- The sentence splitter here is a regex for harness use; the engine uses ICU with clinical suppressions.
- Qwen3 rerankers get their instruct template from `evaluate.py`; the pipeline applies none.
- GenAI `TextRerankPipeline` (2026.3.0 and 2026.3.1) crashes on batched pairs with queries over about 30 words; `--rerank-backend genai` is for parity checks only.
- Gold files: `rag/gold/<set>/*.jsonl`, fields in `gold.py`.
- Exports run optimum-cli with TEMP under `rag/candidates/.tmp`: quantisation writes an fp32 copy to TEMP first, and C: has no room for a large model. A failed export is removed and reported at the end; the rest continue.
