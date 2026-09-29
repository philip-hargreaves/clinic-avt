using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Adapters;

/// <summary>
/// Swaps the host's content between pages. Pages are singletons, so each keeps its state
/// between visits.
/// </summary>
public sealed class NavigationService(IReadOnlyDictionary<string, Func<UIElement>> pages)
    : INavigationService
{
    private string? _current;
    private ContentControl? _host;

    /// <summary>Raised with the page key on every navigation.</summary>
    public event Action<string>? Navigated;

    public void Attach(ContentControl host) => _host = host;

    public void NavigateTo(string pageKey)
    {
        if (_current == pageKey)
        {
            return;
        }
        _current = pageKey;
        Host().Content = pages[pageKey]();
        Navigated?.Invoke(pageKey);
    }

    private ContentControl Host() =>
        _host ?? throw new InvalidOperationException("navigation host not attached");
}
