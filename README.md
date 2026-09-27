# ClinicAVT

On-device ambient AI for clinical consultations. A C++20 engine with a WinUI 3 shell that
listens to the consultation, produces a labelled transcript, and drafts a structured clinical note.

Everything runs locally and nothing is sent over the internet. The recording is not kept. Saved
consultations are stored encrypted on the computer and leave it only when you copy, export or
back them up.

## Run the release package

Requirements: Windows 11 x64, an Intel Core Ultra Series 2 processor or above with an Intel Arc iGPU,
16 GB RAM and 20 GB free disk.

1. Extract `clinicavt.zip` anywhere, for example a folder on the Desktop.
2. Open the `clinicavt` folder and double-click `ClinicAVT.App.exe`.

If Windows SmartScreen objects to an unsigned download, choose "More info", then
"Run anyway".

The first launch is slow. Each model is compiled for the machine's GPU and cached, which
can take a few minutes. While that runs, the first playback or recording may stutter, and
in the worst case the app can crash. Close it and open it again. The prepared models are
kept, so it does not repeat. This comes from the zip-style distribution, and an installed
build would prepare the models during installation instead. Every later launch starts
quickly.

## Trying it out

The zip bundles recorded doctor-patient consultations, so you can see the full pipeline
without holding a consultation yourself:

1. Open Settings and turn on **Demo tray**. A replay bar appears along the bottom.
2. Pick **Elbow swelling** and press play. Click the speed button a few times to take it
   to 16x. Even at 16x the nine-minute consultation takes a couple of minutes to
   transcribe, so watch the transcript build and let it run to the end. A real
   consultation arrives at normal speed, which the models transcribe with a lot of
   headroom, so live use keeps up with the conversation throughout.
3. When the replay finishes, the clinical note is written, then the patient sheet.
4. On the note, change the style (Prose or SOAP) and the detail level, and press
   Regenerate to rewrite it.
5. Open Patient information, pick a language, and press Translate.

Recording a real conversation with the microphone works the same way. Press the record
button and speak.

For a showcase, turn on **Demo mode** under Developer tools and choose a consultation. The
record button then plays that track's saved run back in about half a minute. The clock races
through the recording, then the note, guidance and patient sheet the models wrote for it stream in
as they were. It lands in review of a fresh demo copy, so regenerate, translate and reflect all
run for real. A **Demo** chip in the status bar shows while the mode is on, and nothing in a
playback is a measurement. Record the saved runs with `python tools/demo/record_masters.py` while
the app is closed. It plays every bundled track at 1x with the app's note settings. On a demo
record the note header also offers **Example case**. A written case from `demo/cases.txt` stands
in as the note, the guidance search runs on it, and the patient sheet can be rewritten from it.

The zip already contains the model weights.

## Backing up

Settings > Your data, or Back up on the Sessions page, saves the consultations from a period to a
`.clinicavt` file protected by a password. Nobody can open the file without the password, and it
cannot be reset, so keep it somewhere you can find without this computer. After the backup is
checked, Remove these from this computer clears those consultations here. Restore in Settings
brings them back and leaves any already on the computer as they are. A backup holds patient
information: keep it only where your practice allows, and restore only on a computer your practice
has approved.

## Build and run with Visual Studio

You need:

- Visual Studio 2022 or 2026 with the **Desktop development with C++** workload
  (this includes CMake and the MSVC toolchain)
- the .NET 10 SDK

Then, from a developer command prompt in the repo root:

1. Fetch the OpenVINO toolchain the engine builds against and the PDF reader its ingest
   host links:

   ```powershell
   powershell tools\get-openvino.ps1
   powershell tools\get-pdfium.ps1
   ```

   These download the pinned archives (OpenVINO GenAI about 1 GB, PDFium about 4 MB),
   verify their hashes, and install them under `external\` inside the repo. CMake already
   points there, so nothing needs configuring. Run them once per clone. Each is a no-op
   when already installed.

2. Build the engine:

   ```powershell
   cmake --preset release
   cmake --build --preset release
   ```

   The app always launches the *release* engine, even from a Debug shell. A debug engine
   is 5-10x slower through the models, which makes every timing observation misleading.
   You only rebuild it when engine code changes.

3. Put the models in place. Either fetch them:

   ```powershell
   cmake --build --preset release --target fetch-models
   ```

   or copy the `models` folder out of a release zip into the repo root. Both give you
   `models/` with one folder per model.

4. Open `clinicavt.slnx` in Visual Studio. Set `ClinicAVT.App` as the startup project and the
   platform to **x64**, then F5.

The first build creates `models`, `prompts` and `demo` junctions beside the exe, pointing
back into the repo. A prompt edit applies to the next note without a rebuild, and the build
never copies the model store. The build output lives at
`app\ClinicAVT.App\bin\x64\Debug\net10.0-windows10.0.26100.0\win-x64\`.

If the app starts but reports that the engine or a model is missing, repeat step 2 or
step 3. The app tells you which.

To build everything from the command line instead: `dotnet build clinicavt.slnx -p:Platform=x64`.

## Models

Weights are not in git. They ship as GitHub Release assets described by the `weights/`
registry, with a SHA-256 per file and shards of up to 1.9 GiB. One command downloads,
verifies and installs the packs at the top of `weights/` into `models/` and `demo/`:

```powershell
cmake --build --preset dev --target fetch-models
```

Interrupted downloads resume on re-run. A hash mismatch is fatal and nothing is installed.

That covers the default note tier. The constrained (4B) and accuracy (35B) note models are
separate packs under `weights/tiers/`, fetched only when wanted:

```powershell
dotnet run --project tools\ClinicAVT.FetchModels -c Release -- fetch weights\tiers\constrained .
dotnet run --project tools\ClinicAVT.FetchModels -c Release -- fetch weights\tiers\accuracy .
```

## Tests

```powershell
cmake --workflow --preset dev
dotnet test clinicavt.slnx
```

The workflow runs configure, build and the engine tests in one step. `dotnet test` builds and
runs the C# suites. The integration tests launch `clinicavt_engine.exe`, so build the engine first.

Unit tests cover the view models and the supervision policy with fakes at the ports, and
run anywhere. Tests that need the built engine carry `Requires=Engine`. The ten-minute replay
at 1x carries `Requires=EngineSlow`, and the crash battery carries `Requires=CrashBattery`. The
fast local run is:

```powershell
dotnet test clinicavt.slnx --filter "Requires!=Engine&Requires!=EngineSlow&Requires!=CrashBattery"
```

CI runs that filter without an engine and `Requires=Engine` after building one. The slow and
crash sets run on demand through `tools/run-gates.ps1`. There is no UI automation. The views
are XAML with thin code-behind, and the logic they bind to is tested through the view models.

## Repository layout

```
engine/            C++20 engine. src/core holds pure logic with one folder per stage, src/ports
                   the interfaces and src/adapters one folder per seam. tests/ mirrors src/
app/               .NET shell. ClinicAVT.App holds the WinUI views, ClinicAVT.App.Core the view
                   models without WinUI, ClinicAVT.App.Platform the Win32 adapters and
                   ClinicAVT.Client the engine SDK over the pipe. Each has a test project
tools/             ClinicAVT.FetchModels for weights download and packing, the toolchain and
                   release scripts, and the internal evaluation harnesses (perf-loop, demo,
                   eval, retrieval, translation)
schema/fixtures/   example wire messages that both the engine and the shell tests read
weights/           model pack manifests. The packs themselves are release assets
prompts/  demo/    note prompts and the bundled demo consultations
```

The shell talks to the engine over a named pipe, and nothing in `engine/` references `app/`.
Both halves are organised by feature within each layer. `engine/src/core/guidance/`,
`engine/src/adapters/guidance/`, `app/ClinicAVT.App.Core/Features/Guidance/` and
`app/ClinicAVT.App/Features/Guidance/` are one feature read across the product. `app/README.md` and
`engine/src/README.md` describe each half.

## Notes

C++ follows the Google C++ Style Guide, with a 4-space indent and a 100 column limit set in
`.clang-format`.

C# follows the standard .NET conventions, with naming and style enforced at build time
through `.editorconfig`.

## License

Distributed under the MIT License. See [`LICENSE`](LICENSE) for the full text.

Copyright (c) 2026 Philip Hargreaves.
