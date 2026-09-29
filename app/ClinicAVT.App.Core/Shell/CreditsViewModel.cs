using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Shell;

public sealed class CreditsViewModel(ICreditsSource credits)
{
    public IReadOnlyList<CreditMark> Marks { get; } = credits.LoadMarks();
}
