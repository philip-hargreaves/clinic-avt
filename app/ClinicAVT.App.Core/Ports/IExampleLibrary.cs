using ClinicAVT.App.Core.Features.Examples;

namespace ClinicAVT.App.Core.Ports;

/// <summary>The example consultations shipped beside the app, read on each call.</summary>
public interface IExampleLibrary
{
    IReadOnlyList<ExampleCase> LoadCases();

    IReadOnlyList<ExampleRecording> LoadRecordings();
}
