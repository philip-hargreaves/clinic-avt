using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Platform;

namespace ClinicAVT.App.Composition;

/// <summary>
/// The launch steps in order. Data comes first and the engine starts once the window shows.
/// </summary>
internal static class StartupTasks
{
    public static IServiceCollection AddStartupTasks(this IServiceCollection services)
    {
        services.AddSingleton<IStartupTask, AttachSessionState>();
        services.AddSingleton<IStartupTask, ApplyTheme>();
        services.AddSingleton<IStartupTask, StartEngine>();
        services.AddSingleton<IStartupTask, RequestMicrophoneAccess>();
        services.AddSingleton<StartupRunner>();
        return services;
    }

    // LiveSessionState mirrors the view model so the engine host can read the session
    private sealed class AttachSessionState(LiveSessionState state, IConsultation session) : IStartupTask
    {
        public string Name => "attach session state";

        public StartupStage Stage => StartupStage.BeforeWindow;

        public void Run() => state.Follow(session);
    }

    // Runs before Activate so a dark preference never flashes light
    private sealed class ApplyTheme(AppPreferences preferences, IThemeService theme) : IStartupTask
    {
        public string Name => "apply theme";

        public StartupStage Stage => StartupStage.AfterWindow;

        public void Run() => theme.Apply(preferences.Theme);
    }

    private sealed class StartEngine(IEngineHost host) : IStartupTask
    {
        public string Name => "start engine";

        public StartupStage Stage => StartupStage.AfterWindow;

        public void Run() => host.Start();
    }

    // Lists the app on the Windows microphone privacy page. The engine enforces the setting
    private sealed class RequestMicrophoneAccess(ILogger<RequestMicrophoneAccess> logger) : IStartupTask
    {
        public string Name => "request microphone access";

        public StartupStage Stage => StartupStage.AfterWindow;

        public void Run()
        {
            if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 18362))
            {
                _ = RequestAsync();
            }
        }

        [System.Runtime.Versioning.SupportedOSPlatform("windows10.0.18362")]
        private async Task RequestAsync()
        {
            try
            {
                await Windows.Security.Authorization.AppCapabilityAccess.AppCapability
                    .Create("microphone").RequestAccessAsync();
            }
            catch (Exception e)
            {
                // The Windows toggle keeps its current setting and the engine still honours it
                logger.StepFailed("microphone access request", e.Message);
            }
        }
    }
}
