using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Shell;

/// <summary>The partner marks shown in the status bar.</summary>
public sealed class CreditsViewModel(ICreditsSource credits)
{
    public IReadOnlyList<CreditMark> Marks { get; } = credits.LoadMarks();
}
