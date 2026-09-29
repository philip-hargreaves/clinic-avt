using System.ComponentModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Appraisal;

namespace ClinicAVT.App.Features.Appraisal;

public sealed partial class ReflectionCardView : UserControl
{
    public static readonly DependencyProperty CardProperty = DependencyProperty.Register(
        nameof(Card), typeof(ReflectionCard), typeof(ReflectionCardView),
        new PropertyMetadata(null, OnCardChanged));

    public ReflectionCardView() => InitializeComponent();

    public ReflectionCard? Card
    {
        get => (ReflectionCard?)GetValue(CardProperty);
        set => SetValue(CardProperty, value);
    }

    public bool Collapsed => Card is { Expanded: false };

    private static void OnCardChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var view = (ReflectionCardView)d;
        if (e.OldValue is ReflectionCard old)
        {
            old.PropertyChanged -= view.OnCardPropertyChanged;
        }

        if (e.NewValue is ReflectionCard card)
        {
            card.PropertyChanged += view.OnCardPropertyChanged;
        }

        view.SyncEditor();
        view.Bindings.Update();
    }

    private void OnCardPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(ReflectionCard.Editor) or nameof(ReflectionCard.Expanded))
        {
            SyncEditor();
            Bindings.Update();
        }
    }

    // The title binds on every keystroke, so the editor holds the final text by LostFocus
    private void OnTitleCommitted(object sender, RoutedEventArgs e)
    {
        if (Card?.Editor is { } editor)
        {
            UiEvent.Run(editor.SaveTitleAsync);
        }
    }

    private void SyncEditor()
    {
        if (Card is not { Editor: { } editor } card)
        {
            EditorHost.Content = null;
            return;
        }

        if (EditorHost.Content is not ReflectionEditorView view || view.ViewModel != editor)
        {
            EditorHost.Content = new ReflectionEditorView(
                editor, showHeading: false, removeCommand: card.DeleteCommand);
        }
    }
}
