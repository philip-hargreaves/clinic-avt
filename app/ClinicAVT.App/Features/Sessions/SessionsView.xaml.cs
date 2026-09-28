using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Sessions;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Features.Documents;

namespace ClinicAVT.App.Features.Sessions;

public sealed partial class SessionsView : UserControl
{
    public SessionsView(SessionsViewModel viewModel, ShellViewModel shell, ReviewSurfaceView surface)
    {
        ViewModel = viewModel;
        Shell = shell;
        InitializeComponent();
        SurfaceHost.Content = surface;
        ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SessionsViewModel.DetailOpen) && ViewModel.DetailOpen)
            {
                surface.Open(preferGuidelines: true);
            }
        };
        Loaded += (_, _) => _ = ViewModel.EnterAsync();
    }

    public SessionsViewModel ViewModel { get; }

    public ShellViewModel Shell { get; }

    // The box binds on every keystroke, so the view model is current by LostFocus
    private void OnTitleCommitted(object sender, RoutedEventArgs e) => UiEvent.Run(ViewModel.RenameAsync);
}
