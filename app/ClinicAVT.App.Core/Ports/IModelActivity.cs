using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Ports;

/// <summary>Model loads, device switches and first-time setup, each timed on the status line.</summary>
public interface IModelActivity : INoteModelLoad
{
    /// <summary>The note lane's state from a notification or a reply. A reply passes null for firstUse.</summary>
    void ApplyNoteModel(ModelState state, bool? firstUse, string? name = null);

    /// <summary>Starts a switch. {time} in the line stands for the elapsed clock.</summary>
    void BeginSwitch(string line);

    void EndSwitch();

    /// <summary>Recording is held while the models compile for this computer.</summary>
    void SetSettingUp(bool settingUp);
}
