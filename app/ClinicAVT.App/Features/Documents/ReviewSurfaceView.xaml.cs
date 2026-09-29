using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Features.Guidance;

namespace ClinicAVT.App.Features.Documents;

/// <summary>
/// The review of one consultation. Wide, the documents and the references get a selector each.
/// Narrow, all five views share one. Each page has its own instance because an element has one parent.
/// </summary>
public sealed partial class ReviewSurfaceView : UserControl
{
    // Below this width everything goes into one selector
    private const double WideThreshold = 1100;

    // The narrow selector's views, in the order they were added
    private const int NarrowGuidelines = 2;
    private const int NarrowTranscript = 3;
    private const int NarrowPage = 4;

    // The reference selector's views
    private const int Guidelines = 0;
    private const int Transcript = 1;
    private const int Page = 2;

    private readonly TranscriptPaneView _transcript;
    private readonly NoteEditorView _note;
    private readonly PatientEditorView _patient;
    private readonly GuidanceSectionView _guidance;
    private readonly PageView _page;
    private readonly PageViewModel _pageView;
    private readonly ContentControl[] _narrow = [Slot(), Slot(), Slot(), Slot(), Slot()];
    private readonly ContentControl[] _documents = [Slot(), Slot()];
    private readonly ContentControl[] _references = [Slot(), Slot(), Slot()];
    private bool? _wide;

    public ReviewSurfaceView(
        TranscriptPaneView transcript, NoteEditorView note, PatientEditorView patient,
        GuidanceSectionView guidance, PageViewModel pageView, PageView page)
    {
        _transcript = transcript;
        _note = note;
        _patient = patient;
        _guidance = guidance;
        _page = page;
        _pageView = pageView;
        InitializeComponent();

        NarrowTabs.Add("Clinical note", _narrow[0]);
        NarrowTabs.Add("Patient information", _narrow[1]);
        NarrowTabs.Add("Guidelines", _narrow[2]);
        NarrowTabs.Add("Transcript", _narrow[3]);
        NarrowTabs.Add("Document page", _narrow[4]);
        DocumentTabs.Add("Clinical note", _documents[0]);
        DocumentTabs.Add("Patient information", _documents[1]);
        ReferenceTabs.Add("Guidelines", _references[0]);
        ReferenceTabs.Add("Transcript", _references[1]);
        ReferenceTabs.Add("Document page", _references[2]);
        Place(wide: false);
        PlaceGuidelines();
        PlacePage();

        _pageView.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(PageViewModel.Visible))
            {
                PlacePage();
            }
        };
        // A page shown while another is open comes to the front too
        _pageView.Shown += PlacePage;
        _guidance.ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(GuidanceViewModel.Visible))
            {
                PlaceGuidelines();
            }
        };
        foreach (var tabs in new[] { NarrowTabs, DocumentTabs, ReferenceTabs })
        {
            tabs.Loaded += (_, _) => Fit();
            tabs.SizeChanged += (_, _) => Fit();
        }
        // The note grows as it streams and the transcript cap follows it
        _note.SizeChanged += (_, _) => Fit();
    }

    /// <summary>
    /// Opens on the note. A stored consultation shows its guidance alongside when it has any. A new
    /// one shows the transcript, which is complete while the note is still streaming.
    /// </summary>
    public void Open(bool preferGuidelines)
    {
        NarrowTabs.SelectedIndex = 0;
        DocumentTabs.SelectedIndex = 0;
        ReferenceTabs.SelectedIndex =
            preferGuidelines && _guidance.ViewModel.Visible ? Guidelines : Transcript;
    }

    private static ContentControl Slot() => new()
    {
        HorizontalContentAlignment = HorizontalAlignment.Stretch,
        VerticalContentAlignment = VerticalAlignment.Stretch,
    };

    private void OnSizeChanged(object sender, SizeChangedEventArgs e) =>
        Place(e.NewSize.Width >= WideThreshold);

    // One set of views, moved between the layouts' slots
    private void Place(bool wide)
    {
        if (_wide == wide)
        {
            return;
        }

        _wide = wide;
        foreach (var slot in _narrow.Concat(_documents).Concat(_references))
        {
            slot.Content = null;
        }
        if (wide)
        {
            _documents[0].Content = _note;
            _documents[1].Content = _patient;
            _references[Guidelines].Content = _guidance;
            _references[Transcript].Content = _transcript;
            _references[Page].Content = _page;
        }
        else
        {
            _narrow[0].Content = _note;
            _narrow[1].Content = _patient;
            _narrow[NarrowGuidelines].Content = _guidance;
            _narrow[NarrowTranscript].Content = _transcript;
            _narrow[NarrowPage].Content = _page;
        }

        WideLayout.Visibility = wide ? Visibility.Visible : Visibility.Collapsed;
        NarrowLayout.Visibility = wide ? Visibility.Collapsed : Visibility.Visible;
        Fit();
    }

    private void PlaceGuidelines()
    {
        var shown = _guidance.ViewModel.Visible;
        NarrowTabs.SetVisible(NarrowGuidelines, shown);
        ReferenceTabs.SetVisible(Guidelines, shown);
    }

    // An open page gets its own tab at the front. Closing it returns the reader to the
    // guidelines it came from
    private void PlacePage()
    {
        var open = _pageView.Visible;
        var narrowOnPage = NarrowTabs.SelectedIndex == NarrowPage;
        var referenceOnPage = ReferenceTabs.SelectedIndex == Page;
        NarrowTabs.SetVisible(NarrowPage, open);
        ReferenceTabs.SetVisible(Page, open);
        if (open)
        {
            NarrowTabs.SelectedIndex = NarrowPage;
            ReferenceTabs.SelectedIndex = Page;
            return;
        }

        var guidelines = _guidance.ViewModel.Visible;
        if (narrowOnPage)
        {
            NarrowTabs.SelectedIndex = guidelines ? NarrowGuidelines : NarrowTranscript;
        }
        if (referenceOnPage)
        {
            ReferenceTabs.SelectedIndex = guidelines ? Guidelines : Transcript;
        }
    }

    // Beside the note the transcript ends level with the note column, but never under half
    // the area. In the single selector it has the whole area
    private void Fit()
    {
        var area = _wide == true ? DocumentTabs.ContentArea : NarrowTabs.ContentArea;
        _note.FitTabContent(area);
        _patient.FitTabContent(area);
        _transcript.CapHeight(_wide == true && area.ActualHeight > 0
            ? Math.Max(_note.ActualHeight, area.ActualHeight * 0.5)
            : double.PositiveInfinity);
    }
}
