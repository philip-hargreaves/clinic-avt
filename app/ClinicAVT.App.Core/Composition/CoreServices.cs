using Microsoft.Extensions.DependencyInjection;
using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Features.Help;
using ClinicAVT.App.Core.Features.Sessions;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Core.Composition;

/// <summary>
/// The view models, one instance each, over whatever ports the host registers. Kept here so
/// a test can build the same graph over fakes and prove every constructor still resolves.
/// </summary>
public static class CoreServices
{
    public static IServiceCollection AddCoreViewModels(this IServiceCollection services)
    {
        services.AddSingleton<LiveSessionState>();
        services.AddSingleton<ISessionState>(sp => sp.GetRequiredService<LiveSessionState>());
        services.AddSingleton<TranscriptViewModel>();
        services.AddSingleton<NoteViewModel>();
        services.AddSingleton<DocumentExportViewModel>();
        services.AddSingleton<GuidanceViewModel>();
        services.AddSingleton<PageViewModel>();
        services.AddSingleton<MicViewModel>();
        services.AddSingleton<ConsultationViewModel>();
        services.AddSingleton<SessionControlsViewModel>();
        services.AddSingleton<ConsultationHeaderViewModel>();
        services.AddSingleton<ShellViewModel>();
        services.AddSingleton<SettingsViewModel>();
        services.AddSingleton<VoiceViewModel>();
        services.AddSingleton<SessionsViewModel>();
        services.AddSingleton<AppraisalsViewModel>();
        services.AddSingleton<HelpViewModel>();
        return services;
    }
}
