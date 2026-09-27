# perf-loop: 1x performance harness

Internal evidence tool, not shipped. Drives the release engine over its pipe with replayed
consults at real-time speed and records per-phase timings, engine memory and every engine
death. Findings and data from the first campaign (2026-08-29) live in the
research repo: `docs/production/latency-probe-2026-08-29.md`.

All working data goes to `build/perf-loop/` (override with `PERF_DIR`); nothing here writes
into the repo.

```
python tools/perf-loop/make_tracks.py            # 2/5/10/15/20-minute tracks from demo/
python tools/perf-loop/perf_loop.py 9            # soak: cycles of all tracks for 9 h
python tools/perf-loop/perf_loop.py 0.01 c02m    # one 2-minute run
python tools/perf-loop/analyse.py                # -> build/perf-loop/report.md
```

Transcript quality gate (method: research repo `docs/production/transcript-quality-gate.md`):

```
# before and after a change, all 57 consults at 16x, transcripts saved per consult
$env:PERF_TAG='-before'; $env:PERF_SAVE='1'; $env:PERF_SPEED='16'; $env:PERF_CYCLES='1'
$env:PERF_AUDIO_DIR='C:\dev\intelliscribe\bench\transcription\mixed'
python tools/perf-loop/perf_loop.py 3
python tools/perf-loop/score_gate.py before after   # WER, negation diffs, attribution
```

Finished studies, kept as evidence:

```
tier_campaign.ps1, tier_report.py    note tiers, stop to last token at 1x
tier_sweep.ps1, baseline_chain.ps1   note tier load sweep and quality baseline (research repo scripts)
label_bench.py, label_e2e.py         session title prompt on the sweep notes, and end to end
ab_test.py, ab_report.py             EcoQoS A/B/C on the engine and note host
```

Rules that the numbers depend on: one model-loading job on the machine at a time; state the
power mode and whether the machine was attended (ADR-0028); create `build/perf-loop/STOP` to
end a loop between runs. `PERF_TAG=-name` keeps an experiment's results in their own files.

The pipe client is `tools/common/engine_pipe.py`, shared with the demo and eval tools. It is
single-threaded on purpose: a synchronous named-pipe handle serialises reads and writes, so it
polls `PeekNamedPipe` and reads only what is waiting. Closing asks the engine to exit and waits;
nothing kills it, since a process stopped mid-GPU can wedge the driver.

Pause and resume: create `build/perf-loop/PAUSE` and the loop waits between runs until it is
deleted; `PERF_SKIP_DONE=1` (with `PERF_SAVE`) skips any track whose transcript is already
saved under the tag, so a killed sweep resumes where it stopped. Never set it for a tag that
repeats one track.
