using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Core.Features.Backup;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Features.Appraisal;
using ClinicAVT.App.Features.Backup;
using ClinicAVT.App.Features.Consultation;
using ClinicAVT.App.Features.Settings;

namespace ClinicAVT.App.Adapters;

public sealed class WinUiDialogService(WindowAccessor window) : IDialogService
{
    // Cancel is the safe default in every confirmation
    public async Task<bool> ConfirmAsync(string title, string content, string primary, string cancel)
    {
        var dialog = Attach(new ContentDialog
        {
            Title = title,
            Content = content,
            PrimaryButtonText = primary,
            CloseButtonText = cancel,
            DefaultButton = ContentDialogButton.Close,
        });
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }

    // The text scrolls, so a large Windows text size never pushes the tick box out of reach
    public async Task<bool?> ConfirmWithOptionAsync(
        string title, string content, string tick, string primary, string cancel)
    {
        var box = new CheckBox { Content = tick };
        var dialog = Attach(new ContentDialog
        {
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
        });
        return await dialog.ShowAsync() == ContentDialogResult.Primary ? box.IsChecked == true : null;
    }

    public async Task<bool> ShowAsync(object viewModel)
    {
        var dialog = Attach(View(viewModel));
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }

    // A dialog opens in the popup layer, outside the page, so it takes the page's theme explicitly
    // or it follows Windows instead of the app
    private ContentDialog Attach(ContentDialog dialog)
    {
        dialog.XamlRoot = window.XamlRoot;
        if (window.Window?.Content is FrameworkElement root)
        {
            dialog.RequestedTheme = root.ActualTheme;
        }

        return dialog;
    }

    private static ContentDialog View(object viewModel) => viewModel switch
    {
        BackupViewModel backup => new BackupDialog(backup),
        RestoreViewModel restore => new RestoreDialog(restore),
        ExportReflectionsViewModel export => new ExportReflectionsDialog(export),
        ImportRecordingViewModel import => new ImportRecordingDialog(import),
        EnrolmentViewModel enrolment => new EnrolmentDialog(enrolment),
        ReflectionViewModel reflection => new ReflectionDialog(reflection),
        _ => throw new ArgumentException($"no dialog for {viewModel.GetType().Name}", nameof(viewModel)),
    };
}
