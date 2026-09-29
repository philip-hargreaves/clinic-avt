using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Graphics;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Adapters;

namespace ClinicAVT.App.Shell;

public sealed partial class MainWindow : Window
{
    public MainWindow(
        NavigationService navigation, ShellViewModel shell, StatusBarViewModel status,
        StatusBarView statusBar)
    {
        Shell = shell;
        Status = status;
        InitializeComponent();
        StatusHost.Content = statusBar;

        navigation.Navigated += Select;
        // The pane opens itself once its template applies, so the closed state is set again
        // after load
        Nav.Loaded += (_, _) => Nav.IsPaneOpen = false;

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        // An unpackaged app shows the window icon in the taskbar and title bar
        AppWindow.SetIcon(System.IO.Path.Combine(
            System.AppContext.BaseDirectory, "Assets", "AppIcon.ico"));

        navigation.Attach(NavHost);
        shell.NavigateCommand.Execute(Routes.Consultation);

        AppWindow.Resize(new SizeInt32(1280, 820));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = 960;
            presenter.PreferredMinimumHeight = 640;
        }
    }

    public ShellViewModel Shell { get; }

    public StatusBarViewModel Status { get; }

    private void OnNavSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        var key = args.IsSettingsSelected
            ? Routes.Settings
            : (args.SelectedItem as NavigationViewItem)?.Tag as string;
        if (key is not null)
        {
            Shell.NavigateCommand.Execute(key);
        }
    }

    private void Select(string key)
    {
        if (key == Routes.Settings)
        {
            Nav.SelectedItem = Nav.SettingsItem;
            return;
        }
        foreach (var item in Nav.MenuItems.Concat(Nav.FooterMenuItems))
        {
            if (item is NavigationViewItem entry && entry.Tag as string == key)
            {
                Nav.SelectedItem = entry;
                return;
            }
        }
    }
}
