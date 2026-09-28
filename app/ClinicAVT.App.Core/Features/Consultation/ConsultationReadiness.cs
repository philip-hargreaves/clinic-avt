using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// Sets up the engine on connect. Covers the first-use compile gate, note options (resent after
/// every restart), translation languages and guidance embedder state.
/// </summary>
public sealed partial class ConsultationReadiness : ObservableObject
{
    private readonly IEngineApi _engine;
    private readonly StatusBarViewModel _status;
    private readonly NoteViewModel _note;
    private readonly GuidanceViewModel _guidance;
    private readonly SessionRecorder _recorder;
    private readonly AppPreferences? _preferences;
    private readonly TimeSpan _pollInterval;
    private bool _checking;

    public ConsultationReadiness(
        IEngineApi engine, StatusBarViewModel status, NoteViewModel note, GuidanceViewModel guidance,
        SessionRecorder recorder, AppPreferences? preferences, TimeSpan pollInterval)
    {
        _engine = engine;
        _status = status;
        _note = note;
        _guidance = guidance;
        _recorder = recorder;
        _preferences = preferences;
        _pollInterval = pollInterval;
    }

    /// <summary>
    /// False only while first-time model compiles run. Blocks recording so the first one avoids
    /// the slow path. Warm launches are never gated.
    /// </summary>
    [ObservableProperty]
    public partial bool ModelsReady { get; private set; } = true;

    public void Connected()
    {
        _ = LoadLanguagesAsync();
        _ = LoadGuidanceReadinessAsync();
        _ = ConfigureThenCheckReadinessAsync();
    }

    // Polled as well as notified, because the embedder loads before the shell connects and
    // its notification can be missed
    public async Task LoadGuidanceReadinessAsync()
    {
        var before = (_guidance.Readiness, _guidance.ReadinessDetail, _guidance.RefusedCorpora.Count);
        if (!await EngineCall.LogAsync(_status, "guidance/corpora",
                async () => _guidance.ApplyCorpora(await _engine.GuidanceCorporaAsync().ConfigureAwait(true)))
            .ConfigureAwait(true))
        {
            _guidance.CorporaUnavailable();
        }

        // Logged on change only, because the poll repeats on every model transition
        if ((_guidance.Readiness, _guidance.ReadinessDetail, _guidance.RefusedCorpora.Count) == before)
        {
            return;
        }

        if (_guidance.ReadinessDetail.Length > 0)
        {
            _status.Log($"guidance unavailable: {_guidance.ReadinessDetail}");
        }

        foreach (var refused in _guidance.RefusedCorpora)
        {
            _status.Log($"guidance corpus refused: {refused}");
        }
    }

    /// <summary>
    /// The note lane's model changed. A first-use compile gates recording while it runs.
    /// </summary>
    public void NoteModelChanged(NoteModelState model)
    {
        // The gate only matters before a recording starts
        if (_recorder.State != SessionState.Idle)
        {
            return;
        }

        if (model.State == "loading" && model.FirstUse)
        {
            ModelsReady = false;
            _status.SetSettingUp(true);
        }
        else if (model.State is "ready" or "failed")
        {
            GateLifted();
        }
    }

    private void GateLifted()
    {
        if (!ModelsReady)
        {
            ModelsReady = true;
            _status.SetSettingUp(false);
            _status.Append("Ready");
        }
    }

    public void NoteOptionsChanged()
    {
        _preferences.Update(p =>
        {
            p.NoteStyle = _note.Style;
            p.NoteDetail = _note.Detail;
        });

        _ = PushNoteOptionsAsync();
    }

    // The tier goes first because readiness reports the configured tier's compile cache
    private async Task ConfigureThenCheckReadinessAsync()
    {
        await PushNoteOptionsAsync().ConfigureAwait(true);
        await CheckReadinessAsync().ConfigureAwait(true);
    }

    // Engine options live per process, so they are sent again after a restart. The tier is
    // a role, which the engine's store resolves
    private async Task PushNoteOptionsAsync()
    {
        if (_engine.Connected)
        {
            // Use the reply's model state, since the shell may reconnect mid-load
            await EngineCall.TryAsync(_status, "note/tier", async () =>
            {
                var reply = await _engine.SetNoteTierAsync(_preferences?.NoteTier ?? AppPreferences.AutoNoteTier)
                    .ConfigureAwait(true);
                _status.ApplyNoteModel(reply.State, firstUse: null, reply.Name);
            }).ConfigureAwait(true);
            await EngineCall.TryAsync(_status, "note/options",
                () => _engine.SetNoteOptionsAsync(_note.Style, _note.Detail)).ConfigureAwait(true);
        }
    }

    // On first launch this polls until the one-off compiles finish. It fails open, so a
    // readiness error cannot block recording
    private async Task CheckReadinessAsync()
    {
        // Every reconnect calls this. One poll loop runs at a time
        if (_checking)
        {
            return;
        }

        _checking = true;
        try
        {
            if (!await EngineCall.LogAsync(_status, "engine/readiness", async () =>
                {
                    var readiness = await _engine.ReadinessAsync().ConfigureAwait(true);
                    // A note host wedged in the GPU driver outlives the engine. Only a reboot
                    // ends it
                    if (readiness.StrayNoteHost)
                    {
                        _status.Append("A note process is stuck in the graphics driver. Use Restart on the power menu to free it");
                        _status.Log("stray note host detected at engine start");
                    }

                    // Recording is refused until the model is installed, so show why up front
                    if (readiness.Missing.Count > 0)
                    {
                        _status.Append($"Recording unavailable: {ModelNames.Missing(readiness.Missing)}");
                    }

                    if (!readiness.FirstUse)
                    {
                        ModelsReady = true;
                        return;
                    }

                    while (!readiness.Ready)
                    {
                        if (ModelsReady)
                        {
                            ModelsReady = false;
                            _status.SetSettingUp(true);
                        }

                        await Task.Delay(_pollInterval).ConfigureAwait(true);
                        readiness = await _engine.ReadinessAsync().ConfigureAwait(true);
                    }

                    GateLifted();
                }).ConfigureAwait(true))
            {
                ModelsReady = true;
                _status.SetSettingUp(false);
            }
        }
        finally
        {
            _checking = false;
        }
    }

    // Empty when the engine ships without a translation model
    private async Task LoadLanguagesAsync() =>
        await EngineCall.LogAsync(_status, "translate/languages", async () =>
        {
            var languages = await _engine.LanguagesAsync().ConfigureAwait(true);
            _note.Languages.Clear();
            foreach (var language in languages)
            {
                _note.Languages.Add(language);
            }
        }).ConfigureAwait(true);
}
