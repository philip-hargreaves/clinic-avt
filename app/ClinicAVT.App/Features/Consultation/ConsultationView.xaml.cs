using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Controls;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Features.Documents;

namespace ClinicAVT.App.Features.Consultation;

public sealed partial class ConsultationView : UserControl
{
    public ConsultationView(
        SessionControlsView controls, ReviewSurfaceView surface, MicViewModel mic,
        ConsultationHeaderViewModel header)
    {
        Controls = controls.ViewModel;
        Mic = mic;
        Header = header;
        InitializeComponent();
        ControlsHost.Content = controls;
        SurfaceHost.Content = surface;
        AudioDrop.Attach(Root, DropHighlight, () => Controls.ImportRecordingCommand.CanExecute(null),
            path => Controls.ImportRecordingCommand.ExecuteAsync(path));

        // Open the review on the transcript; guidance is not ready until the note finishes
        Controls.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SessionControlsViewModel.PanesVisible) && Controls.PanesVisible)
            {
                surface.Open(preferGuidelines: false);
            }
        };
    }

    public SessionControlsViewModel Controls { get; }

    public MicViewModel Mic { get; }

    public ConsultationHeaderViewModel Header { get; }

    // Refreshes on open so a headset plugged in a moment ago appears
    private void OnMicFlyoutOpening(object sender, object e) => UiEvent.Run(async () =>
    {
        await Mic.RefreshCommand.ExecuteAsync(null);
        BuildMicFlyout();
    });

    private void BuildMicFlyout()
    {
        MicFlyout.Items.Clear();
        if (!Mic.HasDevices)
        {
            MicFlyout.Items.Add(MenuItems.Caption(Mic.NoDevicesText));
            return;
        }

        foreach (var row in Mic.Rows)
        {
            MicFlyout.Items.Add(MenuItems.Radio(row.Label, "mic", row.IsChecked, Mic.SelectCommand, row.Id));
            if (row.NoteVisible)
            {
                MicFlyout.Items.Add(MenuItems.Caption(row.Note));
            }
        }
    }
}
