using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using ClinicAVT.App.Composition;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Composition;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Platform;
using ClinicAVT.App.Shell;

namespace ClinicAVT.App;

public partial class App : Application
{
    private readonly ServiceProvider _services;
    private Window? _window;
    private bool _closing;

    public App()
    {
        // Before anything opens the app's files, so a second launch never races the first
        if (!SingleInstance.Claim())
        {
            SingleInstance.ShowOther();
            Environment.Exit(0);
        }

        InitializeComponent();
        var paths = AppPaths.Default;
        // A debug build checks at launch that every registration can be built
        _services = new ServiceCollection()
            .AddPlatform(paths)
            .AddEngine(paths)
            .AddCore()
            .AddViews()
            .AddStartupTasks(paths)
            .BuildServiceProvider(new ServiceProviderOptions
            {
                ValidateOnBuild = BuildFlags.Debug,
                ValidateScopes = BuildFlags.Debug,
            });
        UiEvent.Logger = _services.GetRequiredService<ILogger<App>>();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        var startup = _services.GetRequiredService<StartupRunner>();
        startup.Run(StartupStage.BeforeWindow);
        _window = _services.GetRequiredService<MainWindow>();
        startup.Run(StartupStage.AfterWindow);
        _window.AppWindow.Closing += OnClosing;
        _window.Activate();
    }

    // Cancel the first close to confirm shutdown. CloseAsync closes the window
    private void OnClosing(AppWindow sender, AppWindowClosingEventArgs e)
    {
        if (!_closing)
        {
            e.Cancel = true;
            _ = CloseAsync();
        }
    }

    private async Task CloseAsync()
    {
        var shutdown = _services.GetRequiredService<AppShutdown>();
        if (_closing || !await shutdown.ConfirmAsync())
        {
            return;
        }

        _closing = true;
        await shutdown.StopAsync();
        _window?.Close();
        _services.Dispose();
    }
}
