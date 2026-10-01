# Performance

How long does the app take from stop to finished note on the target laptop, and does it stay
up over a night of consultations? How fast does each note model decode, and in how much memory?

## Data

Replay tracks cut from the example consultations by `make_tracks.py`, the mixed PriMock57
tracks for sweeps, and `hpi-prompt.txt` with PriMock57 transcripts for the model benchmarks.

## Engine at real time

`perf_loop.py` starts the release engine with `--allow-replay`, which the app never passes,
replays consultations over the pipe at 1x, and records the time of each step, engine memory and
any engine exit. Working data goes to `build/perf-loop/`.

```
python evaluation/performance/make_tracks.py            # 2 to 20-minute tracks
python evaluation/performance/perf_loop.py 9            # soak: all tracks for 9 hours
python evaluation/performance/perf_loop.py 0.01 c02m    # one 2-minute run
python evaluation/performance/analyse.py                # build/perf-loop/report.md
```

A sweep saves each transcript for the diarisation stage to score:

```
$env:PERF_TAG='-before'; $env:PERF_SAVE='1'; $env:PERF_AUDIO_DIR='<mixed tracks>'
python evaluation/performance/perf_loop.py 3
```

| Variable | Effect |
|---|---|
| `PERF_SPEED`, `PERF_CYCLES` | Replay speed and number of cycles |
| `PERF_NOTE_TIER` | Note tier |
| `PERF_ENGINE_ARGS` | Extra engine flags |
| `PERF_SKIP_DONE=1` | Resumes a stopped sweep. Not for a tag that repeats a track. |

Create `build/perf-loop/PAUSE` to pause between runs, or `STOP` to end.

## Note models

```
python evaluation/performance/decode_bench.py <model>          # decode rate, one JSON line
python evaluation/performance/latency.py --prepare-only        # the three inputs
python evaluation/performance/latency.py <model>...            # load, first token, decode, memory
```

`tier_campaign.ps1`, `tier_sweep.ps1` and `ab_test.py` are finished studies, kept with their
report scripts.

## Rules

Run one model-loading job at a time, and leave the machine alone during a run. Accept a result
only after a 1x run, because faster replay hides contention. Record the power mode. The pipe
client never kills the engine, because a process stopped mid-GPU can hang the driver.
