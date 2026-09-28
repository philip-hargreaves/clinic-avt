using Microsoft.Extensions.Logging;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Core.Features.Backup;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Features.Appraisal;
using ClinicAVT.App.Features.Backup;
using ClinicAVT.App.Features.Consultation;
using ClinicAVT.App.Features.Settings;
using ClinicAVT.Client;

namespace ClinicAVT.App.Platform;

public sealed class WinUiDialogService(
    WindowAccessor window, IEngineApi engine, IUiDispatcher dispatcher, MicViewModel mic,
    StatusBarViewModel status, IClipboard clipboard, IFilePicker picker, ILauncher launcher,
    AppPreferences preferences, TimeProvider clock, ISessionState session,
    ILogger<WinUiDialogService> logger) : IDialogService
{
    // Cancel is the safe default in every confirmation
    public async Task<bool> ConfirmAsync(string title, string content, string primary, string cancel)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = window.XamlRoot,
            Title = title,
            Content = content,
            PrimaryButtonText = primary,
            CloseButtonText = cancel,
            DefaultButton = ContentDialogButton.Close,
        };
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }

    // The text scrolls, so a large Windows text size never pushes the tick box out of reach
    public async Task<bool?> ConfirmWithOptionAsync(
        string title, string content, string tick, string primary, string cancel)
    {
        var box = new CheckBox { Content = tick };
        var dialog = new ContentDialog
        {
            XamlRoot = window.XamlRoot,
            Title = title,
            Content = new ScrollViewer
            {
                Content = new StackPanel
                {
                    Spacing = 12,
                    Children = { new TextBlock { Text = content, TextWrapping = TextWrapping.Wrap }, box },
                },
            },
            PrimaryButtonText = primary,
            CloseButtonText = cancel,
            DefaultButton = ContentDialogButton.Close,
        };
        return await dialog.ShowAsync() == ContentDialogResult.Primary ? box.IsChecked == true : null;
    }

    public async Task<bool> RunBackupAsync()
    {
        using var backup = new BackupViewModel(engine, picker, launcher, preferences, dispatcher, clock,
            session: session, logger: logger);
        var dialog = new BackupDialog(backup) { XamlRoot = window.XamlRoot };
        _ = backup.LoadAsync();
        await dialog.ShowAsync();
        return backup.RemovedAny;
    }

    public async Task<bool> RunRestoreAsync()
    {
        using var restore = new RestoreViewModel(engine, picker, preferences, dispatcher, clock, logger);
        var dialog = new RestoreDialog(restore) { XamlRoot = window.XamlRoot };
        await dialog.ShowAsync();
        return restore.RestoredAny;
    }

    public async Task RunExportReflectionsAsync()
    {
        var export = new ExportReflectionsViewModel(engine, picker, launcher, clock, logger);
        var dialog = new ExportReflectionsDialog(export) { XamlRoot = window.XamlRoot };
        _ = export.LoadAsync();
        await dialog.ShowAsync();
    }

    // Add closes the dialog; the consultation page then shows the finalise
    public async Task<RecordingImport?> RunImportAsync(string? path)
    {
        var import = new ImportRecordingViewModel(engine, picker, clock, logger: logger);
        var dialog = new ImportRecordingDialog(import) { XamlRoot = window.XamlRoot };
        if (path is not null)
        {
            _ = import.UseFileAsync(path);
        }

        return await dialog.ShowAsync() == ContentDialogResult.Primary ? import.Result : null;
    }

    // New view model per dialog. True when a voiceprint was saved
    public async Task<bool> RunEnrolmentAsync()
    {
        using var enrolment = new EnrolmentViewModel(engine, mic.MicId, dispatcher: dispatcher, logger: logger);
        var dialog = new EnrolmentDialog(enrolment) { XamlRoot = window.XamlRoot };
        await dialog.ShowAsync();
        return await enrolment.Outcome;
    }

    public async Task ShowReflectionAsync(string sessionId, string startedAt)
    {
        // The dialog disposes the view model as it closes. The using covers a load that fails first
        using var reflection = new ReflectionViewModel(engine, dispatcher, clipboard, picker, this, status);
        await reflection.LoadAsync(sessionId, startedAt);
        var dialog = new ReflectionDialog(reflection) { XamlRoot = window.XamlRoot };
        await dialog.ShowAsync();
    }
}
