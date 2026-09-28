# performance

**Question.** How fast is the pipeline on the target laptop, from stop to finished note, and does it
stay up over a night of consultations? How fast does each note model decode, and within what memory?

**Data.** Replay tracks cut from the bundled demo consultations (`make_tracks.py`, into
`build/perf-loop/audio`), the 57 mixed PriMock tracks for sweeps, PriMock transcripts for the model
latency inputs, `hpi-prompt.txt` (frozen), and the long rheumatology consultation for the decode
bench (`sample_consult` in `evaluation/config.toml`).

**Backs.** Research docs `docs/evaluation/6-performance/` (performance-report, performance-model,
architecture-efficiency, speculative-decoding, summariser-performance-benchmarks),
`docs/production/note-tiers.md`, `latency-probe-2026-08-29.md`, ADR-0028 and ADR-0030.

## Engine at 1x

Drives the release engine over its pipe with replayed consultations at real time and records
per-phase timings, engine memory and every engine death. Working data goes to `build/perf-loop/`
(`perf_loop` in the config, override `EVAL_PERF_LOOP`; the engine binary with `EVAL_ENGINE`).

```
python evaluation/performance/make_tracks.py            # 2/5/10/15/20-minute tracks from demo/
python evaluation/performance/perf_loop.py 9            # soak: cycles of all tracks for 9 h
python evaluation/performance/perf_loop.py 0.01 c02m    # one 2-minute run
python evaluation/performance/analyse.py                # -> build/perf-loop/report.md
```

Sweeps (transcripts saved per consultation, scored by the diarisation stage):

```
$env:PERF_TAG='-before'; $env:PERF_SAVE='1'; $env:PERF_SPEED='16'; $env:PERF_CYCLES='1'
$env:PERF_AUDIO_DIR='C:\dev\intelliscribe\bench\transcription\mixed'
python evaluation/performance/perf_loop.py 3
```

`PERF_NOTE_TIER` picks the note tier, `PERF_ENGINE_ARGS` adds engine flags. Create
`build/perf-loop/PAUSE` to hold between runs and `STOP` to end; `PERF_SKIP_DONE=1` (with
`PERF_SAVE`) resumes a killed sweep. Never set it for a tag that repeats one track.

Finished studies, kept as evidence:

```
tier_campaign.ps1, tier_report.py    note tiers, stop to last token at 1x
tier_sweep.ps1                       note tier load sweep through the real note host, then decode_bench.py
ab_test.py, ab_report.py             EcoQoS A/B/C on the engine and note host
```

## Note models alone

```
python evaluation/performance/decode_bench.py qwen3.5-9b-int4 [--tokens 300 --reps 2]    one JSON line
python evaluation/performance/latency.py --prepare-only                                  the three inputs
python evaluation/performance/latency.py qwen3.5-4b-int4 gemma-4-e4b-it-int4-ov          -> build/evaluation/performance/latency.jsonl
```

`latency.py` gives cold and warm load, first token, decode rate, time to a 500-token note against
the 90 s bar, peak RAM and bandwidth efficiency (`params_b`/`active_b` in the config) on the
25th-percentile, median and longest PriMock consultations.

## Rules the numbers depend on

One model-loading job on the machine at a time. Evaluate at 1x before accepting a result; faster
replay hides contention. State the power mode and whether the machine was attended. The pipe
client (`evaluation/common/engine_pipe.py`) is single-threaded on purpose and never kills the engine:
a process stopped mid-GPU can wedge the driver.
