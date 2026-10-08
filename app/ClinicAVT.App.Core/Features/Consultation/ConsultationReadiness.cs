using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// Sets up the engine on connect. Covers the first-use compile gate, note options (resent after
/// every restart), translation languages and guidance embedder state.
/// </summary>
public sealed partial class ConsultationReadiness : ObservableObject
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(2);

    private readonly IEngineControl _engine;
    private readonly INoteApi _notes;
    private readonly IGuidanceApi _guidanceApi;
    private readonly IStatusLine _status;
    private readonly IModelActivity _models;
    private readonly NoteViewModel _note;
    private readonly PatientSheetViewModel _patient;
    private readonly GuidanceAvailability _guidance;
    private readonly SessionRecorder _recorder;
    private readonly AppPreferences _preferences;
    private readonly TimeProvider _time;
    private bool _checking;

    public ConsultationReadiness(
        IEngineControl engine, INoteApi notes, IGuidanceApi guidanceApi, IStatusLine status,
        IModelActivity models, NoteViewModel note, PatientSheetViewModel patient,
        GuidanceAvailability guidance, SessionRecorder recorder, AppPreferences preferences,
        TimeProvider time)
    {
        _engine = engine;
        _notes = notes;
        _guidanceApi = guidanceApi;
        _status = status;
        _models = models;
        _note = note;
        _patient = patient;
        _guidance = guidance;
        _recorder = recorder;
        _preferences = preferences;
        _time = time;
        note.OptionsChanged += NoteOptionsChanged;
    }

    /// <summary>
    /// False until the engine's first readiness reply, and while first-time model compiles run.
    /// Blocks recording so the first one avoids the slow path.
    /// </summary>
    [ObservableProperty]
    public partial bool ModelsReady { get; private set; }

    private bool _settingUp;

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
                async () => _guidance.ApplyCorpora(await _guidanceApi.GuidanceCorporaAsync().ConfigureAwait(true)))
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

    /// <summary>A first-use compile gates recording while it runs.</summary>
    public void NoteModelChanged(NoteModelState model)
    {
        // The gate only matters before a recording starts
        if (_recorder.State != SessionState.Idle)
        {
            return;
        }

        if (model.State == ModelState.Loading && model.FirstUse)
        {
            BeginSetup();
        }
        else if (model.State is (ModelState.Ready or ModelState.Failed) && _settingUp)
        {
            GateLifted();
        }
    }

    private void BeginSetup()
    {
        ModelsReady = false;
        if (!_settingUp)
        {
            _settingUp = true;
            _models.SetSettingUp(true);
        }
    }

    // Also clears activity an engine restart interrupted. A resumed recording or a readiness
    // warning keeps its own line
    private void GateLifted(bool announce = true)
    {
        ModelsReady = true;
        if (_settingUp)
        {
            _settingUp = false;
            _models.SetSettingUp(false);
        }

        if (announce && _recorder.State == SessionState.Idle)
        {
            _status.Append("Ready");
        }
    }

    private void NoteOptionsChanged()
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
                var tier = await InstalledNoteTierAsync().ConfigureAwait(true);
                var reply = await _engine.SetNoteTierAsync(tier).ConfigureAwait(true);
                _models.ApplyNoteModel(reply.State, firstUse: null, reply.Name);
            }).ConfigureAwait(true);
            await EngineCall.TryAsync(_status, "note/options",
                () => _notes.SetNoteOptionsAsync(_note.Style, _note.Detail)).ConfigureAwait(true);
        }
    }

    // The data folder outlives an uninstall, so a saved tier may not be installed. It goes back
    // to automatic instead of being refused
    private async Task<string> InstalledNoteTierAsync()
    {
        var tier = _preferences.NoteTier;
        if (tier == AppPreferences.AutoNoteTier)
        {
            return tier;
        }

        var models = await _engine.ListModelsAsync().ConfigureAwait(true);
        if (models.Any(m => m.Task == ModelTask.Note && m.Tier == tier))
        {
            return tier;
        }

        _preferences.Update(p => p.NoteTier = AppPreferences.AutoNoteTier);
        return AppPreferences.AutoNoteTier;
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

                    var warned = readiness.StrayNoteHost || readiness.Missing.Count > 0;
                    while (readiness.FirstUse && !readiness.Ready)
                    {
                        BeginSetup();
                        await Task.Delay(PollInterval, _time).ConfigureAwait(true);
                        readiness = await _engine.ReadinessAsync().ConfigureAwait(true);
                    }

                    GateLifted(announce: !warned);
                }).ConfigureAwait(true))
            {
                GateLifted();
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
            var languages = await _notes.LanguagesAsync().ConfigureAwait(true);
            _patient.Languages.Clear();
            foreach (var language in languages)
            {
                _patient.Languages.Add(language);
            }
        }).ConfigureAwait(true);
}
