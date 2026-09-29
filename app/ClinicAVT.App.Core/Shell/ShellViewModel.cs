using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Shell;

/// <summary>Page navigation, including what leaving and entering each page triggers.</summary>
public sealed partial class ShellViewModel(INavigationService navigation, IEnumerable<INavigationGuard> guards)
    : ObservableObject
{
    private readonly IReadOnlyList<INavigationGuard> _guards = [.. guards];
    private string? _current;

    /// <summary>
    /// Shows the page with that key. Leaving Appraisal saves whatever reflection is open.
    /// Consultation means recording a new one, so a stored review ends before the page shows.
    /// </summary>
    [RelayCommand]
    public async Task NavigateAsync(string key)
    {
        foreach (var guard in _guards)
        {
            await guard.OnNavigatingAsync(_current, key).ConfigureAwait(true);
        }

        _current = key;
        navigation.NavigateTo(key);
    }

    [RelayCommand]
    private Task ShowSettings() => NavigateAsync(Routes.Settings);
}
