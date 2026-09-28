using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// The microphone picker. It fetches the engine's list each time it opens. The choice is
/// stored by id and falls back to another device only while that one is gone.
/// </summary>
public sealed partial class MicViewModel : ObservableObject
{
    private const string BluetoothNote = "    Bluetooth call mode - reduced recording quality";

    private readonly IEngineApi _engine;
    private readonly AppPreferences? _preferences;
    private readonly ILogger? _logger;

    // Empty means "the system default", which the engine pins
    private string _selectedId;

    public MicViewModel(IEngineApi engine, AppPreferences? preferences = null,
        IUiDispatcher? dispatcher = null, ILogger<MicViewModel>? logger = null)
    {
        _engine = engine;
        _preferences = preferences;
        _logger = logger;
        _selectedId = preferences?.MicId ?? "";
        // Refresh at connect so the label is right before the first open
        if (dispatcher is not null)
        {
            engine.OnConnected(dispatcher, () => _ = RefreshAsync());
        }
    }

    public ObservableCollection<MicDevice> Devices { get; } = [];

    public ObservableCollection<MicRow> Rows { get; } = [];

    public bool HasDevices => Devices.Count > 0;

    public string NoDevicesText { get; } = "No microphone found - connect one to record";

    /// <summary>The id session/start pins. Empty means the system default.</summary>
    public string MicId => Current?.Id ?? "";

    /// <summary>
    /// The shortest name that still picks out one device, since several mics can all report
    /// "Microphone". It tries the endpoint, then the endpoint with its adapter, then the full
    /// name.
    /// </summary>
    public string Label
    {
        get
        {
            if (Current is not { } current)
            {
                return "No microphone found";
            }

            var endpoint = Endpoint(current);
            if (Devices.Count(d => Endpoint(d) == endpoint) == 1)
            {
                return endpoint;
            }

            return Devices.Count(d => d.ShortName == current.ShortName) == 1
                ? current.ShortName
                : current.Name;
        }
    }

    /// <summary>The full name, so the trimmed label is never the only identification.</summary>
    public string FullName => Current?.Name ?? "No microphone found";

    private MicDevice? Current =>
        Devices.FirstOrDefault(d => d.Id == _selectedId)
        ?? Devices.FirstOrDefault(d => d.IsDefault)
        ?? Devices.FirstOrDefault();

    /// <summary>Reloads the device list. Called when the picker opens.</summary>
    [RelayCommand]
    public async Task RefreshAsync()
    {
        if (!_engine.Connected)
        {
            return;
        }

        try
        {
            var inputs = await _engine.ListAudioInputsAsync().ConfigureAwait(true);
            Devices.Clear();
            foreach (var device in inputs)
            {
                Devices.Add(new MicDevice(
                    device.Id, device.Name ?? "Microphone", device.ShortName ?? "Microphone",
                    device.IsDefault, device.Bluetooth));
            }
        }
        catch (Exception e)
        {
            // A failed refresh keeps the last list. The engine still resolves the device
            _logger?.StepFailed("audio/inputs", e.Message);
        }

        Changed();
    }

    /// <summary>The clinician's pick, kept for every future consultation.</summary>
    [RelayCommand]
    public void Select(string id)
    {
        _selectedId = id;
        _preferences.Update(p => p.MicId = id);

        Changed();
    }

    private void Changed()
    {
        var current = MicId;
        Rows.Clear();
        foreach (var device in Devices)
        {
            Rows.Add(new MicRow(
                device.IsDefault ? $"{device.Name}  (default)" : device.Name,
                device.Bluetooth ? BluetoothNote : "",
                device.Id == current,
                device.Id));
        }

        OnPropertyChanged(nameof(Label));
        OnPropertyChanged(nameof(FullName));
        OnPropertyChanged(nameof(HasDevices));
        OnPropertyChanged(nameof(MicId));
    }

    private static string Endpoint(MicDevice device)
    {
        var cut = device.ShortName.IndexOf(" on ", StringComparison.OrdinalIgnoreCase);
        return cut > 0 ? device.ShortName[..cut] : device.ShortName;
    }
}
