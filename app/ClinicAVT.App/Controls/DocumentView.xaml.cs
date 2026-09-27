using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using ClinicAVT.App.Themes;

namespace ClinicAVT.App.Controls;

/// <summary>A document to read. Editing swaps in a DocumentTextBox.</summary>
public sealed partial class DocumentView : UserControl
{
    public static readonly DependencyProperty TextProperty = DependencyProperty.Register(
        nameof(Text), typeof(string), typeof(DocumentView),
        new PropertyMetadata("", (d, _) => ((DocumentView)d).Body.TextHighlighters.Clear()));

    public static readonly DependencyProperty LabelProperty = DependencyProperty.Register(
        nameof(Label), typeof(string), typeof(DocumentView), new PropertyMetadata(""));

    public DocumentView() => InitializeComponent();

    public string Text
    {
        get => (string)GetValue(TextProperty);
        set => SetValue(TextProperty, value);
    }

    /// <summary>The document's accessible name.</summary>
    public string Label
    {
        get => (string)GetValue(LabelProperty);
        set => SetValue(LabelProperty, value);
    }

    /// <summary>
    /// Highlights the first occurrence of a passage, leaving the reader's selection alone. An
    /// empty or missing passage clears the mark.
    /// </summary>
    public void Mark(string passage)
    {
        Body.TextHighlighters.Clear();
        var at = passage.Length > 0 ? Text.IndexOf(passage, StringComparison.Ordinal) : -1;
        if (at < 0)
        {
            return;
        }

        var highlighter = new TextHighlighter
        {
            Background = ThemedResources.GetBrush("ReferenceChipSoftBrush", ActualTheme),
        };
        highlighter.Ranges.Add(new TextRange { StartIndex = at, Length = passage.Length });
        Body.TextHighlighters.Add(highlighter);
    }
}
