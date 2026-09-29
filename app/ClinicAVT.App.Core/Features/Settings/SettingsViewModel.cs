namespace ClinicAVT.App.Core.Features.Settings;

public sealed class SettingsViewModel(
    NoteModelSettings noteModel, GuidanceCorporaViewModel corpora, GuidanceDocumentsViewModel documents,
    PrivacySettings privacy, AppearanceAndDiagnostics appearance)
{
    public NoteModelSettings NoteModel { get; } = noteModel;

    public GuidanceCorporaViewModel Corpora { get; } = corpora;

    public GuidanceDocumentsViewModel Documents { get; } = documents;

    public PrivacySettings Privacy { get; } = privacy;

    public AppearanceAndDiagnostics Appearance { get; } = appearance;
}
