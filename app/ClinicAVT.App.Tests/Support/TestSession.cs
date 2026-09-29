using Microsoft.Extensions.DependencyInjection;
using ClinicAVT.App.Core.Features.Examples;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Support;

internal static class TestSession
{
    /// <summary>A consultation over a quiet fake engine, with its lines going to the log when one is given.</summary>
    public static TestShell Create(
        AppPreferences? preferences = null, FakeDialogService? dialogs = null,
        FakeEngineClient? engine = null, IReadOnlyList<ExampleCase>? exampleCases = null,
        ListLogger? log = null, Func<string, bool>? logged = null)
    {
        var shell = new TestShell(engine, preferences, log: log, logged: logged, configure: services =>
        {
            if (dialogs is not null)
            {
                services.AddSingleton<IDialogService>(dialogs);
            }

            var library = new FakeExampleLibrary();
            library.Cases.AddRange(exampleCases ?? []);
            services.AddSingleton<IExampleLibrary>(library);
        });
        // Built first, as the app's main window builds it before anything else is shown
        _ = shell.Session;
        return shell;
    }

    /// <summary>
    /// The app's graph with default preferences, for the pages that change them. A given session
    /// stands in for the consultation.
    /// </summary>
    public static TestShell Settings(
        FakeEngineClient? engine = null, AppPreferences? preferences = null, ISessionState? session = null,
        FakeDialogService? dialogs = null, FakeFilePicker? picker = null) =>
        new(engine, preferences ?? new AppPreferences(new MemoryPreferencesStore()), configure: services =>
        {
            if (session is not null)
            {
                services.AddSingleton(session);
            }

            if (dialogs is not null)
            {
                services.AddSingleton<IDialogService>(dialogs);
            }

            if (picker is not null)
            {
                services.AddSingleton<IFilePicker>(picker);
            }
        });

    /// <summary>
    /// The status bar's graph over an engine that is not connected, so it hears nothing until a
    /// test raises it.
    /// </summary>
    public static TestShell Offline(ListLogger? log = null, Func<string, bool>? logged = null)
    {
        var silent = new FakeEngineClient();
        silent.SetConnected(false);
        return new TestShell(silent, log: log, logged: logged);
    }
}
