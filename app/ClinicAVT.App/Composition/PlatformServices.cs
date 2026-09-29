using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Adapters;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Platform;

namespace ClinicAVT.App.Composition;

internal static class PlatformServices
{
    public static IServiceCollection AddPlatform(this IServiceCollection services, AppPaths paths)
    {
        services.AddLogging(logging => logging.AddProvider(new FileLoggerProvider(paths.ShellLog)));
        services.AddSingleton(TimeProvider.System);
        services.AddSingleton<IUiDispatcher, UiDispatcher>();
        services.AddSingleton<WindowAccessor>();
        services.AddSingleton<FocusReturn>();
        services.AddSingleton<IClipboard, WinUiClipboard>();
        services.AddSingleton<IFilePicker, WinUiFilePicker>();
        services.AddSingleton<ILauncher, WinUiLauncher>();
        services.AddSingleton<IThemeService, WinUiThemeService>();
        services.AddSingleton<IDialogService, WinUiDialogService>();
        services.AddSingleton<IMachineInfoProvider, WmiMachineInfoProvider>();
        services.AddSingleton<IAppInfo, AppInfo>();
        services.AddSingleton<IProcessMetrics, ProcessMetrics>();
        services.AddSingleton<IPowerStateReader, PowerStateReader>();
        services.AddSingleton<IOneDriveFolders, OneDriveFolders>();
        services.AddSingleton<ITextFiles, TextFiles>();
        services.AddSingleton<IPreferencesStore>(_ => new FilePreferencesStore(paths.Preferences));
        services.AddSingleton<IMetricsLog>(_ => new FileMetricsLog(paths.Metrics));
        services.AddSingleton<IExampleLibrary>(sp => new ExampleLibrary(
            AppContext.BaseDirectory, sp.GetRequiredService<ILogger<ExampleLibrary>>()));
        services.AddSingleton<ICreditsSource>(_ => new CreditsFile(
            Path.Combine(AppContext.BaseDirectory, "Assets", "logos")));
        return services;
    }
}
