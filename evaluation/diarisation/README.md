# Diarisation

Is each word given to the right speaker, is the doctor named without a wrong guess, and does
the transcript stay readable?

## Data

The PriMock57 references and mixed tracks from the transcription stage. The sweep scripts read
transcripts saved by `evaluation/performance/perf_loop.py` with `PERF_SAVE=1`.
`reference/research-port-slices.txt` is the output of the pipeline the engine was ported from.

## Run

The engine offline, through `diar_eval_runner`:

| Script | Checks |
|---|---|
| `runner_accuracy.py` | The engine against the reference pipeline. Passes within 0.5 points. |
| `runner_roles.py` | Doctor naming. Passes on zero wrong names. |

Two sweeps compared, as `<script> <tagA> <tagB>`:

| Script | Measures |
|---|---|
| `score_gate.py` | WER, negations lost or added, word attribution |
| `absorbed_answers.py` | Short answers placed under the other speaker |
| `short_turn_recall.py` | Short reference turns recovered |
| `readability_stats.py` | Broken bigrams, turn shape, duplicated spans |
| `negation_audit.py` | Blinded reading of each negation difference |
| `diff_pairs.py` | Blinded pairwise judging of the regions that differ |

The other scripts are one-off tools, each described in its docstring. Word attribution and
WER cannot see scrambled word order, so read the transcripts as well.
