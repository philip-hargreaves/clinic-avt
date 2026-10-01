# Shell

The .NET 10 shell is the WinUI 3 app. It shows the consultation, its documents and the
settings, and talks to the engine over a named pipe.

## Projects

Dependencies run one way: `ClinicAVT.App` → `ClinicAVT.App.Core` → `ClinicAVT.Client` → engine.

| Project | Holds | Rule |
|---|---|---|
| `ClinicAVT.App` | Views, themes and controls | The only project that references WinUI |
| `ClinicAVT.App.Core` | View models and the ports they depend on | No WinUI, so tests construct view models directly |
| `ClinicAVT.App.Platform` | Win32 implementations of Core's ports | References Core, never WinUI |
| `ClinicAVT.Client` | Typed JSON-RPC client for the engine | Knows nothing about the app |

## Layout

```
app/
├── ClinicAVT.App/
│   ├── Shell/               main window and status bar
│   ├── Features/            one folder per page
│   ├── Controls/            reusable controls
│   ├── Adapters/            WinUI implementations of Core's ports
│   └── Themes/              tokens and styles
├── ClinicAVT.App.Core/
│   ├── Shell/               shell and status view models
│   ├── Features/            view models, mirroring ClinicAVT.App/Features
│   ├── Ports/               interfaces implemented outside Core
│   └── Hosting/             starting and supervising the engine
├── ClinicAVT.App.Platform/
├── ClinicAVT.Client/
├── TestSupport/             helpers shared by the test projects
└── *.Tests/                 xUnit suites
```

The features are Consultation, Documents, Guidance, Sessions, Appraisal, Settings and Help.

## Tests

| Filter | Runs |
|---|---|
| `Requires!=Engine&Requires!=CrashBattery` | Unit tests against fakes, with no engine |
| `Requires=Engine` | Tests that start the built engine |
| `Requires=CrashBattery` | The crash set, run by `tools\run-gates.ps1` |
