using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Tests.TestDoubles;

internal sealed class RecordingNavigationService : INavigationService
{
    public string? Current { get; private set; }

    public void NavigateTo(string pageKey) => Current = pageKey;
}
