using System.ComponentModel;

namespace ClinicAVT.App.Core.Ports;

/// <summary>Whether any guideline document is still being read into the index.</summary>
public interface IGuidelineIndexing : INotifyPropertyChanged
{
    bool Indexing { get; }
}
