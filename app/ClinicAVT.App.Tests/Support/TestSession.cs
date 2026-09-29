using Microsoft.Extensions.DependencyInjection;
using ClinicAVT.App.Core.Features.Examples;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Support;

internal static class TestSession
{
    /// <summary>The consultation over a fake engine that sends no notifications unprompted. Log lines go to the given log.</summary>
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
    /// The app's graph with default preferences. A given session replaces the consultation's
    /// state.
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
    /// The app's graph over a disconnected engine, so no notification arrives until a test raises
    /// one.
    /// </summary>
    public static TestShell Offline(ListLogger? log = null, Func<string, bool>? logged = null)
    {
        var silent = new FakeEngineClient();
        silent.SetConnected(false);
        return new TestShell(silent, log: log, logged: logged);
    }
}
