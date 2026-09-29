using System.ComponentModel;

namespace ClinicAVT.App.Core.Ports;

/// <summary>A note model load in progress, with its elapsed time.</summary>
public interface INoteModelLoad : INotifyPropertyChanged
{
    bool ModelLoading { get; }

    /// <summary>Time since the load began, as 0:48.</summary>
    string ModelLoadElapsed { get; }

    /// <summary>The line describing the load, empty when none runs.</summary>
    string ModelLoadLine { get; }
}
