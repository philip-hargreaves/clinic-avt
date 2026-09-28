using Microsoft.Extensions.DependencyInjection;
using Microsoft.UI.Xaml;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Features.Appraisal;
using ClinicAVT.App.Features.Consultation;
using ClinicAVT.App.Features.Documents;
using ClinicAVT.App.Features.Guidance;
using ClinicAVT.App.Features.Help;
using ClinicAVT.App.Features.Sessions;
using ClinicAVT.App.Features.Settings;
using ClinicAVT.App.Platform;
using ClinicAVT.App.Shell;

namespace ClinicAVT.App.Composition;

/// <summary>
/// Pages are singletons because the navigation service keeps them on its back stack, so their
/// view model subscriptions last as long as the app. Views inside a page are transient because
/// an element can have only one parent.
/// </summary>
internal static class ViewServices
{
    public static IServiceCollection AddViews(this IServiceCollection services)
    {
        services.AddTransient<SessionControlsView>();
        services.AddTransient<TranscriptPaneView>();
        services.AddTransient<GuidanceSectionView>();
        services.AddTransient<PageView>();
        services.AddTransient<NoteEditorView>();
        services.AddTransient<PatientEditorView>();
        services.AddTransient<ReviewSurfaceView>();
        services.AddTransient<StatusBarView>();
        services.AddSingleton<ConsultationView>();
        services.AddSingleton<SessionsView>();
        services.AddSingleton<AppraisalsView>();
        services.AddSingleton<SettingsView>();
        services.AddSingleton<HelpView>();
        // The platform adapters reach the window through the accessor from the moment it exists
        services.AddSingleton(sp =>
        {
            var window = ActivatorUtilities.CreateInstance<MainWindow>(sp);
            sp.GetRequiredService<WindowAccessor>().Window = window;
            return window;
        });
        services.AddSingleton<AppShutdown>();
        services.AddSingleton(sp => new NavigationService(new Dictionary<string, Func<UIElement>>
        {
            [Routes.Consultation] = sp.GetRequiredService<ConsultationView>,
            [Routes.Sessions] = sp.GetRequiredService<SessionsView>,
            [Routes.Appraisals] = sp.GetRequiredService<AppraisalsView>,
            [Routes.Help] = sp.GetRequiredService<HelpView>,
            [Routes.Settings] = sp.GetRequiredService<SettingsView>,
        }));
        services.AddSingleton<INavigationService>(sp => sp.GetRequiredService<NavigationService>());
        return services;
    }
}
