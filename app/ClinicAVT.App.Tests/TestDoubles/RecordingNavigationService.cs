using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Tests.TestDoubles;

/// <summary>Records navigation calls without a real content host.</summary>
internal sealed class RecordingNavigationService : INavigationService
{
    public string? Current { get; private set; }

    public void NavigateTo(string pageKey) => Current = pageKey;
}
