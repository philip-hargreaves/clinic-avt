using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Help;

namespace ClinicAVT.App.Features.Help;

/// <summary>The guide to using the app, and the small print about it.</summary>
public sealed partial class HelpView : UserControl
{
    public HelpView(HelpViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public HelpViewModel ViewModel { get; }
}
