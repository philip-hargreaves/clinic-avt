using System.ComponentModel;

namespace ClinicAVT.App.Core.Ports;

public interface INoteModelLoad : INotifyPropertyChanged
{
    bool ModelLoading { get; }

    /// <summary>Time since the load began, as 0:48.</summary>
    string ModelLoadElapsed { get; }

    /// <summary>The line describing the load, empty when none runs.</summary>
    string ModelLoadLine { get; }
}
