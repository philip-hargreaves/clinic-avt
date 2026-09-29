using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Controls;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Examples;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Preferences;

namespace ClinicAVT.App.Features.Documents;

public sealed partial class NoteEditorView : UserControl
{
    private readonly TabFit _fit;

    public NoteEditorView(
        NoteViewModel viewModel, ReviewCommandsViewModel commands, ExampleCasesViewModel examples,
        DocumentExportViewModel export, GuidanceViewModel guidance)
    {
        ViewModel = viewModel;
        Commands = commands;
        Examples = examples;
        Export = export;
        InitializeComponent();
        _fit = new TabFit(NoteHost, 0.5);
        guidance.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(GuidanceViewModel.Hovered))
            {
                LightSentence(guidance.Hovered);
            }
        };
        ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(NoteViewModel.NoteEditing))
            {
                EditingChrome.Show(NoteBox, ViewModel.NoteEditing);
            }
            else if (e.PropertyName is nameof(NoteViewModel.Style) or nameof(NoteViewModel.Detail))
            {
                CheckOptions();
            }
        };
        BuildOptionsMenu();
    }

    public NoteViewModel ViewModel { get; }

    public ReviewCommandsViewModel Commands { get; }

    public ExampleCasesViewModel Examples { get; }

    public DocumentExportViewModel Export { get; }

    // The note takes the area less the rows around it
    public void FitTabContent(FrameworkElement area)
    {
        var chrome = StateRow.ActualHeight + ActionRow.ActualHeight
            + 2 * Editor.RowSpacing + Editor.Padding.Top + Editor.Padding.Bottom;
        _fit.FitWithin(area, chrome);
    }

    private void BuildOptionsMenu()
    {
        foreach (var option in ViewModel.StyleOptions)
        {
            OptionsMenu.Items.Add(OptionItem("style", option, value => ViewModel.Style = value));
        }
        OptionsMenu.Items.Add(new MenuFlyoutSeparator());
        foreach (var option in ViewModel.DetailOptions)
        {
            OptionsMenu.Items.Add(OptionItem("detail", option, value => ViewModel.Detail = value));
        }
        OptionsMenu.Opening += (_, _) => CheckOptions();
        CheckOptions();
    }

    private static RadioMenuFlyoutItem OptionItem(string group, NoteOption option, Action<string> choose) =>
        MenuItems.Radio(option.Name, group, isChecked: false, () => choose(option.Value), tag: option.Value);

    // Checking an item unchecks the rest of its group. Runs again on open because an item
    // checked before it first shows may not draw its mark
    private void CheckOptions()
    {
        foreach (var entry in OptionsMenu.Items)
        {
            if (entry is RadioMenuFlyoutItem item
                && item.Tag as string == (item.GroupName == "style" ? ViewModel.Style : ViewModel.Detail))
            {
                item.IsChecked = true;
            }
        }
    }

    // Marks the hovered card's sentence in the note being read
    private void LightSentence(string sentence)
    {
        if (!ViewModel.NoteEditing)
        {
            NoteView.Mark(sentence);
        }
    }
}
