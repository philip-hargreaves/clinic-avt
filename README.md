<div align="center">

# ClinicAVT

**On-device ambient voice technology for clinical consultations**

Transcribes and diarises consultations, drafts clinical notes with translation, and retrieves relevant guidelines by semantic search.

[![CI](https://img.shields.io/github/actions/workflow/status/philip-hargreaves/clinic-avt/ci.yml?branch=main&label=CI)](https://github.com/philip-hargreaves/clinic-avt/actions/workflows/ci.yml)
![Platform](https://img.shields.io/badge/platform-Windows%2011-0078D4)
![Engine](https://img.shields.io/badge/engine-C%2B%2B20-00599C)
![Shell](https://img.shields.io/badge/shell-WinUI%203%20%7C%20.NET%2010-512BD4)
![Inference](https://img.shields.io/badge/inference-OpenVINO%20GenAI-0071C5)
![Licence](https://img.shields.io/badge/licence-MIT-green)

</div>

---

ClinicAVT was developed as an MSc project at UCL in collaboration with Intel and the NHS. It is a
research prototype for evaluation and demonstration: every draft must be checked by a clinician
before use.

## Features

<table>
<tr><td><strong>Private by design</strong></td><td>All models run on the device. No audio, text or usage data leaves the computer, and audio is discarded once processed.</td></tr>
<tr><td><strong>Speaker-attributed transcripts</strong></td><td>Speech is transcribed with Whisper and each turn is attributed to the clinician or the patient. Optional voice enrolment improves attribution over time.</td></tr>
<tr><td><strong>Clinical notes</strong></td><td>Structured notes in SOAP form, or in narrative form at two lengths, from a choice of three local language models sized to the hardware.</td></tr>
<tr><td><strong>Patient information</strong></td><td>A plain-English summary for the patient, written to a UK reading age of about 9, with translation into 24 languages including right-to-left scripts.</td></tr>
<tr><td><strong>Guideline references</strong></td><td>Local guideline documents are indexed and searched against each consultation, with the relevant passages shown beside the note.</td></tr>
<tr><td><strong>Appraisal reflections</strong></td><td>Consultations can be summarised and reflected on for appraisal, then exported as text.</td></tr>
<tr><td><strong>Encrypted storage and backups</strong></td><td>Saved consultations are encrypted at rest. Password-protected backups can hold full consultations or reflections only.</td></tr>
<tr><td><strong>Recording import</strong></td><td>Existing audio files can be imported, and five example consultations are bundled for demonstration.</td></tr>
</table>

## Pipeline

```mermaid
flowchart LR
    A[Microphone<br/>or audio file] --> B[Voice activity<br/>Silero VAD]
    B --> C[Speaker diarisation<br/>pyannote + ERes2NetV2]
    B --> D[Transcription<br/>Whisper Large v3 Turbo]
    C --> E[Attributed<br/>transcript]
    D --> E
    E --> F[Clinical note<br/>Qwen 4B / 9B / 35B]
    F --> G[Patient information]
    G --> H[Translation<br/>NLLB-200]
    F --> I[Guideline search<br/>GTE-large]
```

Transcription and note generation run on the integrated Intel GPU, with transcription optionally on
the NPU. The smaller models run on the CPU. Every model runs through OpenVINO, and the larger ones
use 8-bit or 4-bit weights.

## Architecture

```mermaid
flowchart LR
    subgraph Shell["WinUI 3 shell (C#)"]
        UI[Views] --> VM[View models]
    end
    subgraph Engine["Engine (C++20)"]
        Core[Core pipeline] --> Ports[Ports]
        Ports --> Adapters[Adapters]
    end
    VM <-->|JSON-RPC over a named pipe| Core
    Adapters --> OV[OpenVINO GenAI<br/>GPU / NPU / CPU]
    Adapters --> Store[(Encrypted<br/>SQLite store)]
    Adapters --> Hosts[Note and document<br/>host processes]
```

The engine follows a ports-and-adapters design and has no dependency on the shell, so each half is
built and tested independently. Note generation and document parsing run in separate host
processes, which keeps a fault in either away from the recording.

## Models

| Task | Model | Precision | Device |
|---|---|---|---|
| Speech recognition | Whisper Large v3 Turbo | INT8 | GPU or NPU |
| Voice activity | Silero VAD | FP32 | CPU |
| Speaker segmentation | pyannote segmentation 3.0 | FP32 | CPU |
| Speaker identification | ERes2NetV2 | INT8 | CPU |
| Note writing | Qwen3.5 4B, Qwen3.5 9B, Qwen3.6 35B-A3B | INT4 | GPU |
| Translation | NLLB-200 distilled 600M | INT8 | CPU |
| Guideline search | GTE-large | INT8 | CPU |

The note model is chosen automatically: the 9B on computers with enough memory, otherwise the 4B.
The 35B is for high-memory systems and is not in the standard release.

## Requirements

- Windows 11, 64-bit
- Intel Core Ultra Series 2 or later with Intel Arc graphics
- 16 GB RAM (24 GB for the 9B note model, 32 GB for the 35B)
- 25 GB free disk space

## Getting started

1. Update the Intel graphics driver and, in Intel Graphics Software, set **Shared GPU Memory
   Override** to the maximum.
2. Download a release and install the `.msix` package. For the zip release, extract it and open
   `ClinicAVT.exe`.
3. To try it without recording, choose **Import the file** and select an example consultation.

The first launch compiles the models for the computer's graphics hardware, which takes a few
minutes once. A release includes the 4B and 9B note models.

<details>
<summary><strong>Building from source</strong></summary>

Requires Visual Studio 2022 or 2026 with the **Desktop development with C++** workload and the
.NET 10 SDK. From a developer command prompt in the repository root:

```powershell
# Toolchain: pinned OpenVINO GenAI and PDFium, hash-checked, installed under external\
powershell tools\get-openvino.ps1
powershell tools\get-pdfium.ps1

# Engine
cmake --preset release
cmake --build --preset release

# Models: the standard note model and example recordings, then the optional 4B and 35B packs
cmake --build --preset release --target fetch-models
dotnet run --project tools\ClinicAVT.FetchModels -c Release -- fetch weights\tiers\constrained .
dotnet run --project tools\ClinicAVT.FetchModels -c Release -- fetch weights\tiers\accuracy .

# Shell
dotnet build clinicavt.slnx -p:Platform=x64
```

Or open `clinicavt.slnx` in Visual Studio, set `ClinicAVT.App` as the startup project with the x64
platform, and run. The shell always uses the release engine, since a debug engine distorts timings.
To produce a release, run `tools\stage-release.ps1` for the zip folder or `tools\pack-msix.ps1` for
an unsigned MSIX. Both include the 4B and 9B note models. Pass `-Tiers` to choose others.
Pass `-Publisher` with the subject of the certificate that will sign the MSIX.

</details>

<details>
<summary><strong>Testing</strong></summary>

```powershell
cmake --workflow --preset dev
dotnet test clinicavt.slnx --filter "Requires!=Engine&Requires!=CrashBattery"

# The static checks CI runs
.\tools\check-layering.ps1
.\tools\run-clang-tidy.ps1
ruff check evaluation
```

The first command builds the engine and runs its tests. The second runs the C# tests that need no
engine, with view models tested against fakes at the ports. Tests marked `Requires=Engine` start the
built engine; the crash tests (`CrashBattery`) run through `tools\run-gates.ps1`. The layering check
fails if the engine core or ports include an adapter, OpenVINO, SQLite, Win32 or JSON header;
clang-tidy reads the dev preset's `compile_commands.json`.

</details>

<details>
<summary><strong>Repository layout</strong></summary>

```
ClinicAVT/
├── engine/                      C++20 engine
│   ├── domain/                  its own include root, so core cannot include an adapter
│   │   ├── core/                pure pipeline logic, one folder per stage
│   │   │   ├── common/          shared helpers such as logging, text encoding and the worker thread
│   │   │   ├── audio/           capture ring, level metering and voice enrolment
│   │   │   ├── diarisation/     speaker turns, per-turn decoding and role naming
│   │   │   ├── note/            note gating, labelling and failure handling
│   │   │   ├── guidance/        guideline retrieval and ranking
│   │   │   ├── translate/       patient information translation
│   │   │   ├── archive/         backup and restore
│   │   │   ├── records/         session history, deletion and appraisal reflections
│   │   │   ├── demo/            sample consultations for Seed data
│   │   │   ├── metrics/         timings for the performance report
│   │   │   └── session/         consultation lifecycle, import and the note lane
│   │   └── ports/               interfaces the core depends on
│   ├── src/
│   │   ├── adapters/            implementations: OpenVINO models, audio, storage, IPC
│   │   ├── composition/         command line and model roles for the engine's main
│   │   ├── engine_main.cpp      engine process
│   │   ├── note_host_main.cpp   isolated note-generation process
│   │   └── ingest_host_main.cpp isolated PDF parsing process
│   ├── tests/                   GoogleTest suites mirroring domain/ and src/
│   ├── tools/                   guideline corpus builder and a check of how a PDF is indexed
│   └── licences/                third-party notices shipped with the app
├── app/                         .NET 10 shell
│   ├── ClinicAVT.App/           WinUI 3 views, themes and controls
│   ├── ClinicAVT.App.Core/      view models, one folder per feature, no WinUI dependency
│   ├── ClinicAVT.App.Platform/  Win32 adapters
│   ├── ClinicAVT.Client/        typed JSON-RPC client for the engine
│   ├── TestSupport/             helpers shared by the test projects
│   └── *.Tests/                 xUnit suites for each project
├── launcher/                    ClinicAVT.exe, which starts the app from a release folder
├── evaluation/                  evaluation suites
│   ├── common/                  configuration, engine client and statistics shared by the suites
│   ├── knowledge/               clinical knowledge of candidate note models
│   ├── transcription/           transcription accuracy
│   ├── diarisation/             speaker attribution
│   ├── summarisation/           note quality, including the LLM judge
│   ├── retrieval/               guideline search
│   ├── translation/             translation quality
│   └── performance/             latency and memory across note models
├── schema/                      wire messages shared by engine and shell tests, and the version rule
├── prompts/                     note and patient information prompts
├── demo/                        example recordings, sample consultations for Seed data and test cases
├── rag/                         gold sets for the guideline search evaluation
├── weights/                     model pack manifests with SHA-256 hashes
├── tools/                       toolchain, model fetching, release staging and packaging scripts
├── VERSION                      the version of the engine, the shell and the package
├── .github/workflows/ci.yml     builds, tests and static checks on every pull request
├── CMakeLists.txt               engine and launcher build
└── clinicavt.slnx               shell solution
```

`engine/README.md` and `app/README.md` describe each half in more detail.

</details>

## Licence

The source code is released under the [MIT Licence](LICENSE). Model weights and third-party
libraries keep their own licences, listed in `engine/licences/THIRD-PARTY-NOTICES.txt` and on the
application's About page. The NLLB-200 translation model is licensed for non-commercial use only.
The example recordings, the sample consultations and the speech test fixtures come from PriMock57
(Babylon Health), used under CC BY 4.0.

Copyright © 2026 Philip Hargreaves.
