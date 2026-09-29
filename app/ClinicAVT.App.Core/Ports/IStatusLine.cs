namespace ClinicAVT.App.Core.Ports;

/// <summary>The status line at the foot of the window and the activity log behind it.</summary>
public interface IStatusLine
{
    /// <summary>Shows the line and logs it.</summary>
    void Append(string line, bool busy = false);

    /// <summary>Shows the line without logging it.</summary>
    void Show(string line, bool busy = false);

    /// <summary>Logs the line without showing it.</summary>
    void Log(string line);
}
