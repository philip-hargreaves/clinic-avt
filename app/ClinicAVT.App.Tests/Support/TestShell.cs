using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Composition;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Examples;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.Support;

/// <summary>
/// The app's view-model graph as CoreServices registers it, over a fake engine and fake platform
/// ports. Posted work runs inline. The clock starts at the real time and moves only when a test
/// advances it.
/// </summary>
internal sealed class TestShell : IDisposable
{
    private readonly ServiceProvider _services;

    public TestShell(
        IEngineTransport? engine = null, AppPreferences? preferences = null, TimeProvider? time = null,
        ListLogger? log = null, Func<string, bool>? logged = null, Action<IServiceCollection>? configure = null)
    {
        Transport = engine ?? new FakeEngineClient(autoNotify: false);
        Clock = time as FakeTimeProvider ?? new FakeTimeProvider { Now = DateTimeOffset.UtcNow };
        var services = new ServiceCollection();
        services.AddLogging(logging =>
        {
            if (log is not null)
            {
                logging.AddProvider(new ListLoggerProvider(log, logged));
            }
        });
        services.AddSingleton<IEngineApi>(new EngineApi(Transport));
        services.AddSingleton<IUiDispatcher, InlineDispatcher>();
        services.AddSingleton(time ?? Clock);
        services.AddSingleton<IDialogService, FakeDialogService>();
        services.AddSingleton<IFilePicker, FakeFilePicker>();
        services.AddSingleton<ILauncher, FakeLauncher>();
        services.AddSingleton<IClipboard, FakeClipboard>();
        services.AddSingleton<IThemeService, FakeThemeService>();
        services.AddSingleton<INavigationService, RecordingNavigationService>();
        services.AddSingleton<IEngineHost, FakeEngineHost>();
        services.AddSingleton<IMachineInfoProvider, FixedMachine>();
        services.AddSingleton<IAppInfo, FixedAppInfo>();
        services.AddSingleton<IProcessMetrics, FakeProcessMetrics>();
        services.AddSingleton<IPowerStateReader, FixedPowerState>();
        services.AddSingleton<IOneDriveFolders, FakeOneDriveFolders>();
        services.AddSingleton<ITextFiles, FakeTextFiles>();
        services.AddSingleton<IMetricsLog, FakeMetricsLog>();
        services.AddSingleton<IExampleLibrary, FakeExampleLibrary>();
        services.AddSingleton<ICreditsSource, NoCredits>();
        services.AddSingleton<IPreferencesStore, MemoryPreferencesStore>();
        services.AddCore();
        // Without preferences of its own a test keeps its consultations, as the store expects
        services.AddSingleton(preferences
            ?? new AppPreferences(new MemoryPreferencesStore()) { KeepConsultations = true });
        configure?.Invoke(services);
        Registrations = [.. services];
        _services = services.BuildServiceProvider(
            new ServiceProviderOptions { ValidateOnBuild = true, ValidateScopes = true });
    }

    public IEngineTransport Transport { get; }

    public FakeEngineClient Engine => (FakeEngineClient)Transport;

    public FakeTimeProvider Clock { get; }

    public IReadOnlyList<ServiceDescriptor> Registrations { get; }

    public T Get<T>()
        where T : notnull => _services.GetRequiredService<T>();

    public object GetService(Type type) => _services.GetRequiredService(type);

    /// <summary>A new dialog view model, as its factory builds one.</summary>
    public T Create<T>()
        where T : notnull => Get<Func<T>>()();

    public ConsultationViewModel Session => Get<ConsultationViewModel>();

    public NoteViewModel Note => Get<NoteViewModel>();

    public PatientSheetViewModel Patient => Get<PatientSheetViewModel>();

    public ReviewCommandsViewModel Commands => Get<ReviewCommandsViewModel>();

    public ExampleCasesViewModel Examples => Get<ExampleCasesViewModel>();

    public GuidanceViewModel Guidance => Get<GuidanceViewModel>();

    public GuidanceSearchViewModel Search => Get<GuidanceSearchViewModel>();

    public StatusBarViewModel Status => Get<StatusBarViewModel>();

    public StatusLine Line => Get<StatusLine>();

    public ModelActivity Models => Get<ModelActivity>();

    public ModelChips Chips => Get<ModelChips>();

    public ConsultationActivity Activity => Get<ConsultationActivity>();

    public FakeEngineHost Host => (FakeEngineHost)Get<IEngineHost>();

    public FakeDialogService Dialogs => (FakeDialogService)Get<IDialogService>();

    public FakeFilePicker Picker => (FakeFilePicker)Get<IFilePicker>();

    public FakeLauncher Launcher => (FakeLauncher)Get<ILauncher>();

    public FakeClipboard Clipboard => (FakeClipboard)Get<IClipboard>();

    public AppPreferences Preferences => Get<AppPreferences>();

    public void Deconstruct(out ConsultationViewModel session, out FakeEngineClient engine, out NoteViewModel note)
    {
        session = Session;
        engine = Engine;
        note = Note;
    }

    public void Dispose() => _services.Dispose();

    private sealed class ListLoggerProvider(ListLogger log, Func<string, bool>? logged) : ILoggerProvider
    {
        private static readonly ILogger Silent = Microsoft.Extensions.Logging.Abstractions.NullLogger.Instance;

        public ILogger CreateLogger(string categoryName) =>
            logged is null || logged(categoryName) ? log : Silent;

        public void Dispose()
        {
        }
    }
}
