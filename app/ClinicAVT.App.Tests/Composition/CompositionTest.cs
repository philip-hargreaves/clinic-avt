using Microsoft.Extensions.DependencyInjection;
using ClinicAVT.App.Core.Composition;
using ClinicAVT.App.Core.Metrics;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.Composition;

/// <summary>The view-model graph over fakes, so a new constructor parameter fails here before launch.</summary>
public class CompositionTest
{
    private sealed class FixedMachine : IMachineInfoProvider
    {
        public MachineInfo Describe() => new("cpu", 32, "os", [], null);
    }

    private sealed class FixedAppInfo : IAppInfo
    {
        public string Version => "0.0.0";
    }

    [Fact]
    public void EveryRegisteredServiceResolves()
    {
        var engine = new FakeEngineClient(autoNotify: false);
        var dispatcher = new InlineDispatcher();
        var services = new ServiceCollection();
        services.AddSingleton<IEngineApi>(new EngineApi(engine));
        services.AddSingleton<IUiDispatcher>(dispatcher);
        services.AddSingleton<IDialogService, FakeDialogService>();
        services.AddSingleton<IFilePicker, FakeFilePicker>();
        services.AddSingleton<ILauncher, FakeLauncher>();
        services.AddSingleton<IClipboard, FakeClipboard>();
        services.AddSingleton<IThemeService, FakeThemeService>();
        services.AddSingleton<INavigationService, RecordingNavigationService>();
        services.AddSingleton<IEngineHost, FakeEngineHost>();
        services.AddSingleton<IMachineInfoProvider, FixedMachine>();
        services.AddSingleton<IAppInfo, FixedAppInfo>();
        services.AddSingleton<IProcessMetrics, NoProcessMetrics>();
        services.AddSingleton(TimeProvider.System);
        services.AddSingleton(new AppPreferences(new MemoryPreferencesStore()));
        services.AddSingleton(sp => new PerformanceCollector(
            sp.GetRequiredService<IEngineApi>(), () => false, () => null,
            Path.Combine(Path.GetTempPath(), $"clinicavt-composition-{Guid.NewGuid():N}.jsonl")));
        services.AddSingleton(new StatusBarViewModel(new EngineApi(engine), dispatcher));
        services.AddCoreViewModels();

        using var provider = services.BuildServiceProvider(
            new ServiceProviderOptions { ValidateOnBuild = true, ValidateScopes = true });

        foreach (var descriptor in services)
        {
            Assert.NotNull(provider.GetRequiredService(descriptor.ServiceType));
        }
    }
}
