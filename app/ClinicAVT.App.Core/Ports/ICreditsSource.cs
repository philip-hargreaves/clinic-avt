using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Core.Ports;

/// <summary>The partner marks for the status bar.</summary>
public interface ICreditsSource
{
    IReadOnlyList<CreditMark> LoadMarks();
}
