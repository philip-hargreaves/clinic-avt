using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Core.Features.Backup;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Examples;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Features.Help;
using ClinicAVT.App.Core.Features.Sessions;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Metrics;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Composition;

/// <summary>
/// The view models, one instance each, over whatever ports the host registers. Kept here so
/// a test can build the same graph over fakes and prove every constructor still resolves.
/// </summary>
public static class CoreServices
{
    public static IServiceCollection AddCore(this IServiceCollection services)
    {
        services.AddEngineRoles();
        services.AddSingleton<IEngineEvents, EngineEvents>();
        services.AddSingleton(sp => AppPreferences.Load(
            sp.GetRequiredService<IPreferencesStore>(), sp.GetRequiredService<ILogger<AppPreferences>>()));
        services.AddSingleton<PerformanceCollector>();
        services.AddSingleton<LiveSessionState>();
        services.AddSingleton<ISessionState>(sp => sp.GetRequiredService<LiveSessionState>());
        services.AddStatusBar();
        services.AddConsultation();
        services.AddPages();
        services.AddDialogs();
        return services;
    }

    // Every role resolves to the one EngineApi
    private static void AddEngineRoles(this IServiceCollection services)
    {
        services.AddSingleton<IEngineLink>(sp => sp.GetRequiredService<IEngineApi>());
        services.AddSingleton<IEngineControl>(sp => sp.GetRequiredService<IEngineApi>());
        services.AddSingleton<IRecordingApi>(sp => sp.GetRequiredService<IEngineApi>());
        services.AddSingleton<ISessionStoreApi>(sp => sp.GetRequiredService<IEngineApi>());
        services.AddSingleton<INoteApi>(sp => sp.GetRequiredService<IEngineApi>());
        services.AddSingleton<IGuidanceApi>(sp => sp.GetRequiredService<IEngineApi>());
        services.AddSingleton<IEnrolmentApi>(sp => sp.GetRequiredService<IEngineApi>());
        services.AddSingleton<IReflectionApi>(sp => sp.GetRequiredService<IEngineApi>());
        services.AddSingleton<IArchiveApi>(sp => sp.GetRequiredService<IEngineApi>());
    }

    private static void AddStatusBar(this IServiceCollection services)
    {
        services.AddSingleton<ConsultationActivity>();
        services.AddSingleton<StatusLine>();
        services.AddSingleton<IStatusLine>(sp => sp.GetRequiredService<StatusLine>());
        services.AddSingleton<ModelActivity>();
        services.AddSingleton<IModelActivity>(sp => sp.GetRequiredService<ModelActivity>());
        services.AddSingleton<INoteModelLoad>(sp => sp.GetRequiredService<ModelActivity>());
        services.AddSingleton<EngineState>();
        services.AddSingleton<ModelChips>();
        services.AddSingleton<StatusBarViewModel>();
        services.AddSingleton<CreditsViewModel>();
    }

    private static void AddConsultation(this IServiceCollection services)
    {
        services.AddSingleton<TranscriptViewModel>();
        services.AddSingleton<NoteViewModel>();
        services.AddSingleton<PatientSheetViewModel>();
        services.AddSingleton<ReviewCommandsViewModel>();
        services.AddSingleton<DocumentExportViewModel>();
        services.AddSingleton<ExampleCasesViewModel>();
        services.AddSingleton<GuidanceAvailability>();
        services.AddSingleton<GuidanceViewModel>();
        services.AddSingleton<GuidanceSearchViewModel>();
        services.AddSingleton<PageViewModel>();
        services.AddSingleton<IDocumentPages>(sp => sp.GetRequiredService<PageViewModel>());
        services.AddSingleton<ReviewedSession>();
        services.AddSingleton<ReviewPanes>();
        services.AddSingleton<SessionRecorder>();
        services.AddSingleton<SessionImport>();
        services.AddSingleton<DocumentActions>();
        services.AddSingleton<IReviewActions>(sp => sp.GetRequiredService<DocumentActions>());
        services.AddSingleton<ReviewGuidanceSearch>();
        services.AddSingleton<IGuidanceSearch>(sp => sp.GetRequiredService<ReviewGuidanceSearch>());
        services.AddSingleton<SessionReview>();
        services.AddSingleton<ConsultationReadiness>();
        services.AddSingleton<NotificationRouter>();
        services.AddSingleton<ConsultationViewModel>();
        services.AddSingleton<IConsultation>(sp => sp.GetRequiredService<ConsultationViewModel>());
        services.AddSingleton<MicViewModel>();
        services.AddSingleton<IMicrophoneChoice>(sp => sp.GetRequiredService<MicViewModel>());
        services.AddSingleton<SessionControlsViewModel>();
        services.AddSingleton<ConsultationHeaderViewModel>();
    }

    private static void AddPages(this IServiceCollection services)
    {
        services.AddSingleton<ShellViewModel>();
        services.AddSingleton<AppraisalsViewModel>();
        services.AddSingleton<SessionsViewModel>();
        // Run in this order, so an open reflection is saved before a stored review closes
        services.AddSingleton<INavigationGuard>(sp => sp.GetRequiredService<AppraisalsViewModel>());
        services.AddSingleton<INavigationGuard>(sp => sp.GetRequiredService<SessionsViewModel>());
        services.AddSingleton<NoteModelSettings>();
        services.AddSingleton<GuidanceCorporaViewModel>();
        services.AddSingleton<GuidanceDocumentsViewModel>();
        services.AddSingleton<PrivacySettings>();
        services.AddSingleton<AppearanceAndDiagnostics>();
        services.AddSingleton<SettingsViewModel>();
        services.AddSingleton<VoiceViewModel>();
        services.AddSingleton<HelpViewModel>();
    }

    // A new view model for each dialog
    private static void AddDialogs(this IServiceCollection services)
    {
        services.AddFactory<BackupViewModel>();
        services.AddFactory<RestoreViewModel>();
        services.AddFactory<ExportReflectionsViewModel>();
        services.AddFactory<ImportRecordingViewModel>();
        services.AddFactory<EnrolmentViewModel>();
        services.AddFactory<ReflectionViewModel>();
    }

    // Not tracked by the container, which would hold each disposable instance until the app exits
    private static void AddFactory<T>(this IServiceCollection services)
        where T : class =>
        services.AddSingleton<Func<T>>(sp => () => ActivatorUtilities.CreateInstance<T>(sp));
}
