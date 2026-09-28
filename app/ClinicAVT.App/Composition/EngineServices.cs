using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Platform;
using ClinicAVT.Client;

namespace ClinicAVT.App.Composition;

/// <summary>The engine process, its supervision and the typed connection to it.</summary>
internal static class EngineServices
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);

    public static IServiceCollection AddEngine(this IServiceCollection services, AppPaths paths)
    {
        services.AddSingleton<IEngineLauncher>(sp => new ProcessEngineLauncher(
            AppPaths.EngineExe, stderrPath: paths.EngineLog,
            extraArguments: () => EngineArguments(sp.GetRequiredService<AppPreferences>())));
        services.AddSingleton<ICrashLog>(_ => new FileCrashLog(paths.Crashes));
        services.AddSingleton<IEngineHost>(sp => new EngineSupervisor(
            sp.GetRequiredService<IEngineLauncher>(),
            sp.GetRequiredService<ISessionState>(),
            sp.GetRequiredService<TimeProvider>(),
            sp.GetRequiredService<ICrashLog>(),
            () => sp.GetRequiredService<EngineConnection>().MethodInFlight,
            () => sp.GetRequiredService<IEngineApi>().RequestExitAsync()));
        services.AddSingleton(sp => new EngineConnection(
            sp.GetRequiredService<IEngineHost>(),
            static async (pid, ct) => await PipeTransport.ConnectAsync(
                EngineInfo.PipeName, ConnectTimeout, pid, ct).ConfigureAwait(false),
            sp.GetRequiredService<ILogger<EngineConnection>>()));
        services.AddSingleton<IEngineTransport>(sp => sp.GetRequiredService<EngineConnection>());
        services.AddSingleton<IEngineApi>(sp => new EngineApi(sp.GetRequiredService<EngineConnection>()));
        return services;
    }

    // Read per launch, so a restart picks up a changed preference. The tier lets a first launch
    // compile the note model the shell will ask for
    private static IEnumerable<string> EngineArguments(AppPreferences prefs)
    {
        yield return "--note-tier";
        yield return prefs.NoteTier;

        if (prefs.NpuTranscription)
        {
            yield return "--asr-device";
            yield return "NPU";
        }

        if (BuildFlags.Debug && prefs.IncludeResearchGuidance)
        {
            yield return "--include-research";
        }
    }
}
