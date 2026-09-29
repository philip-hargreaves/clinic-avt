using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Help;

namespace ClinicAVT.App.Features.Help;

public sealed partial class HelpView : UserControl
{
    public HelpView(HelpViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public HelpViewModel ViewModel { get; }
}
