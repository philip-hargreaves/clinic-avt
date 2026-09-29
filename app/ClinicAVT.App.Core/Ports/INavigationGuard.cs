namespace ClinicAVT.App.Core.Ports;

/// <summary>Called on every navigation, before the next page shows.</summary>
public interface INavigationGuard
{
    /// <summary>Leaving is null on the first navigation.</summary>
    Task OnNavigatingAsync(string? leaving, string arriving);
}
