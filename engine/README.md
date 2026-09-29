# The engine

Ports and adapters (ADR-0003). `domain/` holds `core/`, pure logic with no runtime dependency, and
`ports/`, the interfaces core drives. It is its own include root, so a core file that includes an
adapter does not build. `src/` holds `adapters/`, the implementations over OpenVINO, WASAPI, SQLite
and the pipe, and the three executables: the engine, the note worker and the document ingest host.
The corpus builder and indexer (`engine/tools/corpus`) and the units tool (`engine/tools`) are
development tools the engine never runs. Speech reaches text one diarised turn at a time: the
diariser finds the turns, the transcriber decodes each turn's own audio, and there is no other
transcription path. With `--scripted` a model that is not installed gets a stand-in (CI and tests);
without it the engine still starts, `engine/readiness` names the missing roles and consultations
are refused. `--allow-replay` lets `session/start` play a wav as the microphone, for tests and
evaluation only.

```
domain/
  core/            a static library, one folder per stage; namespace = folder
    audio/         capture ring, level meter, resume source, the enrolment sink
    session/       the session controller, transcription, note lane, enrolment
    records/       which sessions the history lists, delete and clear, appraisal reflections
    demo/          seeding the sample year
    diarisation/   speaker regions, per-turn decode, re-split, role naming, transcript tidy
    note/          the note gate, label, summary scrub and load-failure reading
    guidance/      query, ranking and scan; added-document units and page text
    archive/       backup contents and the record rules restore checks
    translate/     punctuation the translator cannot write
    metrics/       counters and throughput
    common/        utf8, strings, ISO 8601 time, version, argv, the log
  ports/           the interfaces core drives, flat, and the store error type
src/
  adapters/        one folder per seam, matching core/ where a stage has one
    audio/ vad/ transcription/ diarisation/ note/ translate/ guidance/ storage/ ipc/ models/
    archive/ demo/
    interfaces/    interfaces between adapters that core never calls: the translator, the
                   guidance retriever, lane and document library, the embedder, note tiers and
                   the recording reader
    pdf/           the PDFium reader, linked only by the ingest host
    system/        GPU lease, awake requests, power throttling, process scan, child
                   processes, executable paths, the Recycle Bin, COM, SHA-256, the stderr log
  composition/     the engine's command line, model roles and guidelines folder, for main
  *_main.cpp       the engine, note host and ingest host
```

Diagnostics go through the log port (`core/common/log.hpp`). Each executable installs the stderr
adapter at startup, and every line keeps its process prefix, such as `clinicavt-engine:`.

Tests mirror this tree under `engine/tests/`: `core/<stage>/`, `adapters/<seam>/`,
with stand-in hosts in `support/`, fixtures in `fixtures/` and the evaluation runner in `tools/`.
Test binaries are split by what they need, not by folder: `core_tests` links only the core, so a
core test that needs an adapter does not build; `engine_tests` runs anywhere, `models_tests` needs
staged weights, `gpu_tests` the Intel GPU, `capture_tests` a microphone;
`ctest -LE 'gpu|models|microphone'` is the CPU-only set. The wire messages both sides test against
are in `schema/`.
