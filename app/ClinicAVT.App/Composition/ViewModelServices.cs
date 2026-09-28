using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Composition;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Metrics;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Platform;
using ClinicAVT.Client;

namespace ClinicAVT.App.Composition;

/// <summary>The preferences, the metrics collector and every view model, one instance each.</summary>
internal static class ViewModelServices
{
    public static IServiceCollection AddViewModels(this IServiceCollection services, AppPaths paths)
    {
        services.AddSingleton(sp => AppPreferences.Load(
            paths.Preferences, sp.GetRequiredService<ILogger<AppPreferences>>()));
        services.AddSingleton(sp => new PerformanceCollector(
            sp.GetRequiredService<IEngineApi>(),
            () => sp.GetRequiredService<AppPreferences>().CollectPerformanceData,
            () => sp.GetRequiredService<IEngineHost>().EnginePid,
            paths.Metrics,
            sp.GetRequiredService<IProcessMetrics>(), PowerStateReader.Read,
            sp.GetRequiredService<ILogger<PerformanceCollector>>()));

        services.AddSingleton(sp => new StatusBarViewModel(
            sp.GetRequiredService<IEngineApi>(), sp.GetRequiredService<IUiDispatcher>(),
            memoryGb: () => sp.GetRequiredService<IProcessMetrics>()
                .CommittedGb(EngineLayout.EngineProcess, EngineLayout.NoteHostProcess),
            logger: sp.GetRequiredService<ILogger<StatusBarViewModel>>()));
        services.AddSingleton<CreditsViewModel>();
        services.AddCoreViewModels();
        return services;
    }
}
