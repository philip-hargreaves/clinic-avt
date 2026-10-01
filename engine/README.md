# Engine

The C++20 engine captures a consultation, transcribes and diarises it, writes the note and
serves the shell over a named pipe. It follows a ports-and-adapters design.

## Layout

```
engine/
├── domain/              its own include root, so core cannot include an adapter
│   ├── core/            pipeline logic, one folder per stage
│   └── ports/           interfaces the core calls
├── src/
│   ├── adapters/        implementations over OpenVINO, WASAPI, SQLite and the pipe
│   ├── composition/     command line and model roles
│   └── *_main.cpp       engine, note host and ingest host
├── tests/               mirrors domain/ and src/
├── tools/               guideline corpus builder, PDF indexing check
└── licences/            third-party notices shipped with the app
```

`adapters/` has one folder per dependency: audio, vad, transcription, diarisation, note,
translate, guidance, storage, archive, ipc, models, pdf and system.

## Executables

| Executable | Purpose |
|---|---|
| `clinicavt_engine` | Runs the pipeline and the pipe server |
| `clinicavt_note_host` | Writes notes in a separate process |
| `clinicavt_ingest_host` | Parses one PDF per run |

## Flags

| Flag | Effect |
|---|---|
| `--scripted` | Uses stand-ins for models that are not installed. For CI and tests. |
| `--allow-replay` | Lets `session/start` play a wav as the microphone. For tests and evaluation. |

## Tests

| Binary | Needs |
|---|---|
| `core_tests` | Nothing. Links only the core. |
| `engine_tests` | Nothing |
| `models_tests` | The model weights |
| `gpu_tests` | The Intel GPU |
| `capture_tests` | A microphone |
| `recording_tests` | Media Foundation |

`ctest -LE "gpu|models|microphone|mediafoundation"` runs the CPU-only set.
