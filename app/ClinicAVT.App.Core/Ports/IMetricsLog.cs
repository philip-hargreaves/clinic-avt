namespace ClinicAVT.App.Core.Ports;

/// <summary>The local performance log, one JSON line per finished session.</summary>
public interface IMetricsLog
{
    void Append(string line);

    /// <summary>Every line, or null when nothing has been logged yet.</summary>
    IReadOnlyList<string>? ReadAll();
}
