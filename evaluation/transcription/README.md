# Transcription

Which Whisper export transcribes UK primary-care consultations best, and what do speed,
memory and quantisation cost?

## Data

PriMock57: 57 mock GP consultations with per-speaker audio and transcripts. Clone it to the
`primock57` path in `evaluation/config.toml`. `mix.py` builds the single-microphone tracks the
app hears.

## Run

```
python evaluation/transcription/mix.py           # mixed tracks
python evaluation/transcription/references.py    # reference transcripts
python evaluation/transcription/transcribe.py    # run each export
python evaluation/transcription/score.py         # WER and CER; --clinical adds clinical recall
```

Outputs go to `build/evaluation/transcription/`.

## Metrics

| Metric | Meaning |
|---|---|
| WER, CER | Word and character error rate, after the same normalisation of both texts |
| Content WER | WER with filler words discounted, unless they answer a question |
| Clinical recall | Concepts, drugs, doses and negations kept, with `--clinical` |

## Models

The exports were made with optimum-intel from the Hugging Face checkpoints.

```
optimum-cli export openvino -m openai/whisper-large-v3-turbo --weight-format int8 <folder>
```

The list is `[transcription] models` in the config. The app ships `whisper-turbo-int8`.
