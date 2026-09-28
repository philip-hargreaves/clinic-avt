# The shell

Four production projects and their tests. The dependency arrow runs one way:

```
ClinicAVT.App  ->  ClinicAVT.App.Core  ->  ClinicAVT.Client  -> (named pipe) ->  engine
```

| Project | Holds | Rule |
|---|---|---|
| `ClinicAVT.App` | XAML views, their code-behind, themes, and the WinUI implementations of Core's ports | The only project that references WinUI. If a file needs a XAML type to compile, it lives here. |
| `ClinicAVT.App.Core` | View models, the ports they depend on, engine supervision, preferences, metrics | No WinUI reference, so every view model is a plain object a test constructs off the UI thread. |
| `ClinicAVT.App.Platform` | The Win32, registry and WMI adapters of Core's ports, such as the engine launcher and job object, crash dumps, machine and power readers, process memory and the file logger | What Core must not know. References Core, never WinUI. |
| `ClinicAVT.Client` | `IEngineApi` and its typed replies, `IEngineTransport`, message framing, the pipe transport | The engine's SDK. Knows nothing about the app. |

## Folders

Both shell projects use the same feature names, so a view and its view model sit at mirrored paths:

```
ClinicAVT.App/                      ClinicAVT.App.Core/
  Shell/       window, status bar    Shell/       shell and status view models
  Features/                          Features/
    Consultation/  the live screen     Consultation/
    Documents/     note, patient       Documents/
                   sheet, transcript
    Guidance/      guideline cards     Guidance/
    Sessions/      stored consults     Sessions/
    Appraisal/     reflections         Appraisal/
    Settings/                          Settings/
    Help/          guide and About
                                       Demo/
  Controls/    reusable, no VM       Ports/       interfaces only, implemented outside Core
               such as page header,  Common/      helpers every feature shares, such as
               tabs, icon label,                  engine call, words, session text and
               busy caption and                   clipboard
               stale notice
  Platform/    WinUI adapters        Hosting/     launching, supervising and connecting to
                                                  the engine process
  Themes/      tokens and styles     Metrics/  Preferences/
```

`ClinicAVT.App.Tests` mirrors `ClinicAVT.App.Core`. Its fakes are in `TestDoubles/` and shared
helpers in `Support/`. Helpers both test projects need (wire fixtures, the silence wav, the engine
locator) live once in `TestSupport/` and are linked into each. Tests that need the built engine carry `Requires=Engine`. They are the whole
of `ClinicAVT.Client.Tests/Contract/` plus the real-engine session and transport-recovery tests in
`App.Tests`. `Requires=CrashBattery` marks the crash set and `Requires=Processes` the tests that
start real processes. The fast filter is `Requires!=Engine&Requires!=CrashBattery`.

The settings page is built on the Community Toolkit's settings controls,
`CommunityToolkit.WinUI.Controls.SettingsControls`. They are the one third-party UI dependency,
and every other control is the platform's.
