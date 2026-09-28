using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Controls;
using ClinicAVT.App.Core.Features.Documents;

namespace ClinicAVT.App.Features.Documents;

public sealed partial class PatientEditorView : UserControl
{
    private readonly TabFit _fitAlone;
    private readonly TabFit _fitShared;
    private FrameworkElement? _area;

    public static FlowDirection Flow(bool rightToLeft) =>
        rightToLeft ? FlowDirection.RightToLeft : FlowDirection.LeftToRight;

    public PatientEditorView(NoteViewModel viewModel, DocumentExportViewModel export)
    {
        ViewModel = viewModel;
        Export = export;
        InitializeComponent();
        _fitAlone = new TabFit(PatientHost, 0.6);
        _fitShared = new TabFit(PatientHost, 0.42);
        ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(NoteViewModel.PatientEditing))
            {
                EditingChrome.Show(PatientBox, ViewModel.PatientEditing);
            }

            if (e.PropertyName == nameof(NoteViewModel.TranslationVisible))
            {
                TranslationRow.Height = ViewModel.TranslationVisible ? GridLength.Auto : new GridLength(0);
                if (_area is not null)
                {
                    FitTabContent(_area);
                }
            }
        };
        PatientHost.SizeChanged += (_, _) => FitTranslation();
        TranslationHeader.SizeChanged += (_, _) => FitTranslation();
    }

    public NoteViewModel ViewModel { get; }

    public DocumentExportViewModel Export { get; }

    // In a tab the sheet takes at most three fifths of the area, or under half beside a
    // translation, which leaves room for the actions
    public void FitTabContent(FrameworkElement area)
    {
        _area = area;
        (ViewModel.TranslationVisible ? _fitShared : _fitAlone).Fit(area);
        FitTranslation();
    }

    // The translation takes the height the sheet and rows leave and scrolls beyond it,
    // so the actions sit right under it
    private void FitTranslation()
    {
        if (_area is null || _area.ActualHeight <= 0 || !ViewModel.TranslationVisible)
        {
            return;
        }

        var sheet = PatientHost.ActualHeight > 0
            ? Math.Min(PatientHost.ActualHeight, PatientHost.MaxHeight)
            : PatientHost.MaxHeight;
        var chrome = HeaderRow.ActualHeight + ActionRow.ActualHeight + TranslationHeader.ActualHeight
            + 3 * Editor.RowSpacing + 4 + Editor.Padding.Top + Editor.Padding.Bottom;
        TranslationView.MaxHeight = Math.Max(TranslationView.MinHeight, _area.ActualHeight - sheet - chrome);
    }
}
