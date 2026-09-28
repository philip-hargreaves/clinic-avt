# diarisation

**Question.** Does the engine attribute each word to the right speaker, name the doctor without a
wrong guess, and keep the transcript readable, including short answers?

**Data.** PriMock57 TextGrids (see `evaluation/transcription/README.md`), the mixed tracks, and sweeps of the
engine over them (`evaluation/performance/perf_loop.py` with `PERF_SAVE`, transcripts in
`build/perf-loop/transcripts/<tag>-<cid>_mixed.json`). `reference/research-port-slices.txt` is the
research C++ port's per-slice output, the 96.94% selection pipeline, frozen.

**Backs.** Research docs `docs/evaluation/3-diarisation/voice-enrolment-eval-2026-09.md`,
`torch-free-port-plan.md` (the port matches research), `docs/production/eval-shipped-vs-single-decode-2026-09.md`,
ADR-0031. The selection itself (ERes2NetV2 + Silero + self-enrolment, 95.9%; held-out Fareez;
AMI) was measured by the research bench, which is not migrated: its data went with the returned SSD.

## Engine offline (diar_eval_runner)

```
python evaluation/diarisation/runner_accuracy.py [--reference-only]   engine vs research port, pass within 0.5 pt
python evaluation/diarisation/runner_roles.py                          cold-start naming (zero wrong) and anchor leave-one-out
```

## Engine sweeps (perf-loop transcripts)

```
python evaluation/diarisation/score_gate.py <tagA> <tagB>            WER, negations lost/added, word attribution
python evaluation/diarisation/absorbed_answers.py <tagA> <tagB> [12]  short answers heard under the other speaker
python evaluation/diarisation/short_turn_recall.py <tagA> <tagB> [5]  short reference turns recovered
python evaluation/diarisation/readability_stats.py <tagA> <tagB>      broken bigrams, turn shape, duplicated spans
python evaluation/diarisation/negation_audit.py pack|merge <tag>      blinded reading of each negation difference
python evaluation/diarisation/diff_pairs.py pack|merge <tagA> <tagB>  blinded pairwise judging of differing regions
python evaluation/diarisation/tidy_offline.py <in> <out>              tidy an existing sweep offline (port of tidy_transcript)
python evaluation/diarisation/resplit_calibrate.py <engine log> <sweep log> <tag>
python evaluation/diarisation/enrol_anchor.py <doctor wav> <anchor.bin> [seconds]
python evaluation/diarisation/anchor_sims.py <tag> [day]  |  anchor_curve.py <cold> <enrolled> <day> <out.png>
```

`ABSORBED_SUBSTANTIVE=1` and `NEG_SUBSTANTIVE=1` count only what a transcriber would keep.
Outputs land beside the sweep in `build/perf-loop`. Word attribution and WER cannot see
scrambled word order: read the transcripts too.
