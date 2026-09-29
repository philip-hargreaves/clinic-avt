using System.Text.Json;

namespace ClinicAVT.Client.Tests;

/// <summary>The fixtures both languages must agree on, one test per family.</summary>
public class FixtureTest
{
    [Fact]
    public void ProtocolFixturesRoundTripThroughTheClientTypes()
    {
        var serialized = JsonSerializer.SerializeToElement(
            new PeerInfo("clinicavt-shell", "0.1.0", Protocol.ProtocolVersion),
            Protocol.JsonOptions);
        var expected = Fixtures.Load("hello-request.json").GetProperty("params");
        Assert.True(JsonElement.DeepEquals(serialized, expected));

        // Against the constants, so drift from the shared fixture fails here
        var result = Fixtures.Load("hello-response.json").GetProperty("result");
        var peer = result.Deserialize<PeerInfo>(Protocol.JsonOptions);
        Assert.Equal(
            new PeerInfo(ExpectedEngine.Name, ExpectedEngine.Version, Protocol.ProtocolVersion), peer);

        var echo = Fixtures.Load("echo-request-nonascii.json");
        var payload = echo.GetProperty("params").GetProperty("payload").GetString();
        Assert.NotNull(payload);
        var reserialized = JsonSerializer.Serialize(new { payload }, Protocol.JsonOptions);
        Assert.Contains("naïve", reserialized, StringComparison.Ordinal);
        Assert.Contains("東京", reserialized, StringComparison.Ordinal);

        var error = Fixtures.Load("error-method-not-found.json").GetProperty("error");
        Assert.Equal(-32601, error.GetProperty("code").GetInt32());
    }

    [Fact]
    public void SessionFixturesNameTheirMethodAndCarryTheirParams()
    {
        var label = Fixtures.Load("session-label.json");
        Assert.Equal("session/label", label.GetProperty("method").GetString());
        Assert.Equal("Elbow swelling", label.GetProperty("params").GetProperty("text").GetString());

        var open = Fixtures.Load("session-open.json");
        Assert.Equal("session/open", open.GetProperty("method").GetString());
        Assert.False(string.IsNullOrEmpty(open.GetProperty("params").GetProperty("id").GetString()));

        var close = Fixtures.Load("session-close.json");
        Assert.Equal("session/close", close.GetProperty("method").GetString());
        Assert.False(close.TryGetProperty("params", out _));

        var level = Fixtures.Load("audio-level.json");
        Assert.Equal("audio.level", level.GetProperty("method").GetString());
        Assert.Equal(0.5, level.GetProperty("params").GetProperty("level").GetDouble());
        Assert.False(level.GetProperty("params").GetProperty("clipped").GetBoolean());

        var interrupted = Fixtures.Load("session-interrupted.json");
        Assert.Equal("session/interrupted", interrupted.GetProperty("method").GetString());
        Assert.Equal("deviceLost", interrupted.GetProperty("params").GetProperty("reason").GetString());
        Assert.False(
            string.IsNullOrEmpty(interrupted.GetProperty("params").GetProperty("detail").GetString()));

        var progress = Fixtures.Load("session-progress.json");
        Assert.Equal("session/progress", progress.GetProperty("method").GetString());
        Assert.Equal("speakers", progress.GetProperty("params").GetProperty("stage").GetString());
    }

    [Fact]
    public void SessionReadbackAndDemoFixturesCarryTheStoredRecords()
    {
        var session = Fixtures.Load("session-list.json")
            .GetProperty("result").GetProperty("sessions")[0];
        Assert.False(string.IsNullOrEmpty(session.GetProperty("label").GetString()));
        Assert.Equal(JsonValueKind.Null, session.GetProperty("editedAt").ValueKind);
        Assert.False(session.GetProperty("demo").GetBoolean());

        var seeded = Fixtures.Load("demo-seed.json").GetProperty("result");
        var cleared = Fixtures.Load("demo-clear.json").GetProperty("result");
        Assert.Equal(seeded.GetProperty("added").GetInt32(), cleared.GetProperty("removed").GetInt32());

        var note = Fixtures.Load("session-note.json").GetProperty("result");
        Assert.False(string.IsNullOrEmpty(note.GetProperty("text").GetString()));
        Assert.Equal("prose", note.GetProperty("style").GetString());
        Assert.Equal("standard", note.GetProperty("detail").GetString());
        Assert.True(DateTimeOffset.TryParse(note.GetProperty("generatedAt").GetString(), out _));
        Assert.True(DateTimeOffset.TryParse(note.GetProperty("editedAt").GetString(), out _));

        var patient = Fixtures.Load("session-patient.json").GetProperty("result");
        Assert.True(DateTimeOffset.TryParse(patient.GetProperty("generatedAt").GetString(), out _));
        Assert.True(DateTimeOffset.TryParse(patient.GetProperty("editedAt").GetString(), out _));
        var translation = patient.GetProperty("translation");
        Assert.Equal("pl", translation.GetProperty("language").GetString());
        Assert.True(DateTimeOffset.TryParse(translation.GetProperty("translatedAt").GetString(), out _));
        Assert.Contains("łokcia", translation.GetProperty("text").GetString(), StringComparison.Ordinal);

        var guidance = Fixtures.Load("session-guidance.json")
            .GetProperty("result").GetProperty("guidance");
        Assert.Equal(JsonValueKind.False, guidance.GetProperty("stale").ValueKind);
        Assert.Equal(1, guidance.GetProperty("version").GetInt32());
        Assert.True(guidance.GetProperty("noteRevision").GetInt64() > 0);
        Assert.Equal(1, guidance.GetProperty("shown").GetArrayLength());
        Assert.Equal(1, guidance.GetProperty("searched").GetArrayLength());
        // The stored record is the ready payload minus what only the wire carries
        Assert.False(guidance.TryGetProperty("id", out _));
        Assert.False(guidance.TryGetProperty("storeError", out _));
    }

    [Fact]
    public void ReflectionFixturesCarryTheThreeAnswersAndAgreeAcrossMethods()
    {
        var result = Fixtures.Load("reflection-get.json").GetProperty("result");
        Assert.False(string.IsNullOrEmpty(result.GetProperty("label").GetString()));
        Assert.False(string.IsNullOrEmpty(result.GetProperty("summary").GetProperty("text").GetString()));
        var reflection = result.GetProperty("reflection");
        foreach (var key in new[] { "happened", "learned", "next" })
        {
            Assert.False(string.IsNullOrEmpty(reflection.GetProperty(key).GetString()), key);
        }

        Assert.True(DateTimeOffset.TryParse(reflection.GetProperty("createdAt").GetString(), out _));
        var reference = reflection.GetProperty("references")[0];
        foreach (var key in new[] { "key", "reference", "title", "link", "source" })
        {
            Assert.False(string.IsNullOrEmpty(reference.GetProperty(key).GetString()), key);
        }

        var listed = Fixtures.Load("reflection-list.json")
            .GetProperty("result").GetProperty("reflections")[0];
        Assert.False(listed.GetProperty("demo").GetBoolean());
        var update = Fixtures.Load("reflection-update.json").GetProperty("params");
        Assert.Equal(update.GetProperty("id").GetString(), listed.GetProperty("id").GetString());
        Assert.Equal(update.GetProperty("learned").GetString(), listed.GetProperty("learned").GetString());
        Assert.Equal(1, update.GetProperty("references").GetArrayLength());
        Assert.True(DateTimeOffset.TryParse(listed.GetProperty("startedAt").GetString(), out _));

        var summary = Fixtures.Load("reflection-summary.json");
        Assert.Equal("reflection/summary", summary.GetProperty("method").GetString());
        Assert.False(summary.TryGetProperty("id", out _), "a notification, not a request");
    }

    [Fact]
    public void GuidanceRetrievalFixturesAreARequestItsTwoOutcomesAndTheEmbedderState()
    {
        var search = Fixtures.Load("guidance-search.json");
        Assert.Equal("guidance/search", search.GetProperty("method").GetString());
        Assert.True(search.TryGetProperty("id", out _), "a request, not a notification");
        Assert.False(
            string.IsNullOrEmpty(search.GetProperty("params").GetProperty("id").GetString()));
        Assert.Equal(3, search.GetProperty("params").GetProperty("limit").GetInt32());

        var failed = Fixtures.Load("guidance-failed.json");
        Assert.Equal("guidance/failed", failed.GetProperty("method").GetString());
        Assert.False(failed.TryGetProperty("id", out _), "a notification, not a request");
        Assert.False(
            string.IsNullOrEmpty(failed.GetProperty("params").GetProperty("id").GetString()));
        Assert.False(
            string.IsNullOrEmpty(failed.GetProperty("params").GetProperty("detail").GetString()));

        var ready = Fixtures.Load("guidance-ready.json");
        Assert.Equal("guidance/ready", ready.GetProperty("method").GetString());
        var record = ready.GetProperty("params");
        Assert.False(string.IsNullOrEmpty(record.GetProperty("id").GetString()));
        Assert.Equal(JsonValueKind.Null, record.GetProperty("storeError").ValueKind);
        Assert.Equal(JsonValueKind.False, record.GetProperty("stale").ValueKind);
        Assert.Equal(1, record.GetProperty("version").GetInt32());
        Assert.True(record.GetProperty("noteRevision").GetInt64() > 0);
        Assert.Equal(0.85, record.GetProperty("floor").GetDouble());
        Assert.Equal(JsonValueKind.False, record.GetProperty("abstained").ValueKind);

        var shown = record.GetProperty("shown");
        Assert.Equal(2, shown.GetArrayLength());
        var structured = shown[0];
        foreach (var key in new[]
                 {
                     "corpus", "chunkId", "code", "number", "title", "section", "text", "url",
                     "lastUpdated", "updateTag", "source", "citation", "trigger",
                 })
        {
            Assert.Equal(JsonValueKind.String, structured.GetProperty(key).ValueKind);
        }

        Assert.StartsWith(
            "https://", structured.GetProperty("url").GetString(), StringComparison.Ordinal);
        Assert.True(DateOnly.TryParse(structured.GetProperty("lastUpdated").GetString(), out _));
        Assert.InRange(structured.GetProperty("score").GetDouble(), 0, 1);

        // A plain-text result has no section, date or tag, a file name for its link and the whole note as its trigger
        var plain = shown[1];
        Assert.Equal("text", plain.GetProperty("source").GetString());
        foreach (var key in new[] { "section", "lastUpdated", "updateTag", "trigger" })
        {
            Assert.Equal("", plain.GetProperty(key).GetString());
        }

        Assert.False(Uri.TryCreate(plain.GetProperty("url").GetString(), UriKind.Absolute, out _));
        foreach (var result in shown.EnumerateArray())
        {
            Assert.Equal(JsonValueKind.Number, result.GetProperty("pages").ValueKind);
        }

        var searched = record.GetProperty("searched");
        Assert.Equal(2, searched.GetArrayLength());
        Assert.Equal(64, searched[0].GetProperty("sha256").GetString()!.Length);
        Assert.Equal(JsonValueKind.Null, searched[0].GetProperty("unavailable").ValueKind);

        // The embedder announces its state, and the corpus listing has both shapes
        var model = Fixtures.Load("guidance-model.json");
        Assert.Equal("guidance/model", model.GetProperty("method").GetString());
        Assert.False(model.TryGetProperty("id", out _), "a notification, not a request");
        Assert.Equal("unavailable", model.GetProperty("params").GetProperty("state").GetString());
        Assert.False(
            string.IsNullOrEmpty(model.GetProperty("params").GetProperty("detail").GetString()));

        var corporaResult = Fixtures.Load("guidance-corpora.json").GetProperty("result");
        Assert.Equal("ready", corporaResult.GetProperty("state").GetString());
        Assert.Equal(JsonValueKind.Null, corporaResult.GetProperty("detail").ValueKind);
        var corpora = corporaResult.GetProperty("corpora");
        Assert.Equal(2, corpora.GetArrayLength());
        Assert.Equal(JsonValueKind.Null, corpora[0].GetProperty("unavailable").ValueKind);
        foreach (var key in new[] { "id", "name", "licence", "attribution" })
        {
            Assert.False(string.IsNullOrEmpty(corpora[0].GetProperty(key).GetString()), key);
        }

        Assert.True(corpora[0].GetProperty("chunks").GetInt32() > 0);
        Assert.True(DateTimeOffset.TryParse(corpora[0].GetProperty("builtAt").GetString(), out _));
        Assert.False(string.IsNullOrEmpty(corpora[1].GetProperty("unavailable").GetString()));
        Assert.Equal("", corpora[1].GetProperty("name").GetString());
        Assert.Equal(JsonValueKind.Null, corpora[1].GetProperty("builtAt").ValueKind);
    }

    [Fact]
    public void GuidanceDocumentFixturesCarryEveryStateTheBoxesAndTheNotifications()
    {
        var listing = Fixtures.Load("guidance-documents.json").GetProperty("result");
        var documents = listing.GetProperty("documents");
        Assert.False(string.IsNullOrEmpty(listing.GetProperty("folder").GetString()));
        Assert.True(listing.GetProperty("found").GetBoolean());
        Assert.True(listing.GetProperty("unsupported").GetInt32() >= 0);
        Assert.Equal(["ready", "indexing", "failed"],
            documents.EnumerateArray().Select(d => d.GetProperty("state").GetString()));
        foreach (var document in documents.EnumerateArray())
        {
            Assert.True(document.GetProperty("id").GetInt64() > 0);
            Assert.False(string.IsNullOrEmpty(document.GetProperty("name").GetString()));
            Assert.False(string.IsNullOrEmpty(document.GetProperty("path").GetString()));
            Assert.True(
                DateTimeOffset.TryParse(document.GetProperty("addedAt").GetString(), out _));
        }

        Assert.True(documents[0].GetProperty("chunks").GetInt32() > 0);
        Assert.Equal(JsonValueKind.Null, documents[0].GetProperty("error").ValueKind);
        Assert.Equal("patientData", documents[2].GetProperty("error").GetString());

        var add = Fixtures.Load("guidance-documents-add.json").GetProperty("result");
        Assert.Equal(1, add.GetProperty("documents").GetArrayLength());
        Assert.Equal(["unsupported", "unreadable"], add.GetProperty("skipped").EnumerateArray()
            .Select(s => s.GetProperty("reason").GetString()));

        var page = Fixtures.Load("guidance-page.json").GetProperty("result");
        Assert.EndsWith(".bmp", page.GetProperty("path").GetString());
        Assert.True(page.GetProperty("width").GetInt32() > 0);
        Assert.True(page.GetProperty("height").GetInt32() > 0);
        Assert.True(page.GetProperty("pages").GetInt32() > 0);
        foreach (var box in page.GetProperty("boxes").EnumerateArray())
        {
            Assert.True(box.GetProperty("page").GetInt32() < page.GetProperty("pages").GetInt32());
            Assert.True(box.GetProperty("left").GetDouble() < box.GetProperty("right").GetDouble());
            Assert.True(box.GetProperty("top").GetDouble() < box.GetProperty("bottom").GetDouble());
            Assert.True(box.GetProperty("bottom").GetDouble() <= 1);
        }

        var opened = Fixtures.Load("guidance-documents-open.json").GetProperty("result");
        Assert.False(string.IsNullOrEmpty(opened.GetProperty("path").GetString()));

        var indexed = Fixtures.Load("guidance-document.json");
        Assert.Equal("guidance/document", indexed.GetProperty("method").GetString());
        Assert.False(indexed.TryGetProperty("id", out _), "a notification, not a request");
        Assert.Equal("ready", indexed.GetProperty("params").GetProperty("state").GetString());

        var progress = Fixtures.Load("guidance-progress.json").GetProperty("params");
        Assert.Equal("preparing", progress.GetProperty("phase").GetString());
        Assert.True(
            progress.GetProperty("done").GetInt32() <= progress.GetProperty("total").GetInt32());

        var changed = Fixtures.Load("guidance-documentsChanged.json");
        Assert.Equal("guidance/documentsChanged", changed.GetProperty("method").GetString());
        Assert.Empty(changed.GetProperty("params").EnumerateObject());
    }

    [Fact]
    public void AStorageFaultCarriesItsDetail()
    {
        var fault = Fixtures.Load("storage-fault.json");
        Assert.Equal("storage/fault", fault.GetProperty("method").GetString());
        Assert.Equal(
            new StorageFault("audio commit: database or disk is full"),
            EngineNotifications.Parse("storage/fault", fault.GetProperty("params")));
    }

    // The shell's requests are compared field for field with the fixtures the engine tests too
    [Fact]
    public async Task BackupFixturesAreWhatTheClientSendsAndReads()
    {
        var transport = new ReplayingTransport();
        var api = new EngineApi(transport);

        var summary = Fixtures.Load("archive-summary.json");
        var summaryParams = summary.GetProperty("request").GetProperty("params");
        var covered = summaryParams.GetProperty("covered");
        transport.Reply = summary.GetProperty("response").GetProperty("result");
        var counts = await api.ArchiveSummaryAsync(
            summaryParams.GetProperty("from").GetString()!, summaryParams.GetProperty("to").GetString()!,
            new ArchiveCoverage(covered.GetProperty("from").GetString()!,
                covered.GetProperty("to").GetString()!, covered.GetProperty("at").GetString()!));
        transport.AssertSent("archive/summary", summaryParams);
        Assert.Equal(new ArchiveSummary(38, 12, 1, 6), counts);

        var backup = Fixtures.Load("archive-backup.json").GetProperty("request");
        var backupParams = backup.GetProperty("params");
        await api.BackUpAsync(backupParams.GetProperty("from").GetString()!,
            backupParams.GetProperty("to").GetString()!, backupParams.GetProperty("path").GetString()!,
            backupParams.GetProperty("password").GetString()!,
            backupParams.GetProperty("reflectionsOnly").GetBoolean());
        transport.AssertSent("archive/backup", backupParams);

        var restore = Fixtures.Load("archive-restore.json").GetProperty("request").GetProperty("params");
        await api.RestoreAsync(restore.GetProperty("path").GetString()!,
            restore.GetProperty("password").GetString()!, restore.GetProperty("dryRun").GetBoolean());
        transport.AssertSent("archive/restore", restore);

        var remove = Fixtures.Load("session-remove.json");
        var removeParams = remove.GetProperty("request").GetProperty("params");
        transport.Reply = remove.GetProperty("response").GetProperty("result");
        var removed = await api.RemoveSessionsAsync(
            removeParams.GetProperty("ids").EnumerateArray().Select(i => i.GetString()!).ToList(),
            removeParams.GetProperty("deleteReflections").GetBoolean());
        transport.AssertSent("session/remove", removeParams);
        Assert.Equal(2, removed);

        var deleteAll = Fixtures.Load("session-deleteAll.json");
        transport.Reply = deleteAll.GetProperty("response").GetProperty("result");
        Assert.Equal(40, await api.DeleteAllSessionsAsync());
        transport.AssertSent("session/deleteAll", deleteAll.GetProperty("request").GetProperty("params"));

        var progress = Parse("archive-progress.json");
        Assert.Equal(new ArchiveProgress(ArchiveJob.Backup, ArchivePhase.Writing, 12, 38), progress);

        var backedUp = Assert.IsType<ArchiveDone>(Parse("archive-done-backup.json"));
        Assert.Equal((ArchiveJob.Backup, false, 38, 12), (backedUp.Job, backedUp.DryRun, backedUp.Consultations, backedUp.Reflections));
        Assert.Equal(2, backedUp.Ids.Count);
        Assert.True(DateTimeOffset.TryParse(backedUp.CreatedAt, out _));

        var dryRun = Assert.IsType<ArchiveDone>(Parse("archive-done-restore.json"));
        Assert.Equal((ArchiveJob.Restore, true, 33, 5), (dryRun.Job, dryRun.DryRun, dryRun.Consultations, dryRun.Skipped));
        Assert.Empty(dryRun.Ids);

        Assert.Equal(new ArchiveFailed(ArchiveJob.Restore, ArchiveError.WrongPassword), Parse("archive-failed.json"));
    }

    [Fact]
    public async Task ImportFixturesAreWhatTheClientSendsAndReads()
    {
        var transport = new ReplayingTransport();
        var api = new EngineApi(transport);

        var inspect = Fixtures.Load("recording-inspect.json");
        var inspectParams = inspect.GetProperty("request").GetProperty("params");
        transport.Reply = inspect.GetProperty("response").GetProperty("result");
        var info = await api.InspectRecordingAsync(inspectParams.GetProperty("path").GetString()!);
        transport.AssertSent("recording/inspect", inspectParams);
        Assert.Equal(new RecordingInfo(760.4, "2026-09-26T13:05:00Z"), info);

        Assert.Equal(new ImportProgress("a1b2c3d4e5f60718293a4b5c6d7e8f90", ImportStage.Transcribing, 60),
            Parse("session-importProgress.json"));
        Assert.Equal(new ImportDone("a1b2c3d4e5f60718293a4b5c6d7e8f90"), Parse("session-imported.json"));
        Assert.Equal(new ImportFailed("a1b2c3d4e5f60718293a4b5c6d7e8f90", "cancelled"),
            Parse("session-importFailed.json"));

        // session/imported completes the call, even when sent before the reply
        var import = Fixtures.Load("session-import.json");
        var importParams = import.GetProperty("request").GetProperty("params");
        var path = importParams.GetProperty("path").GetString()!;
        var startedAt = importParams.GetProperty("startedAt").GetString()!;
        transport.Reply = import.GetProperty("response").GetProperty("result");
        transport.Then.AddRange([Fixtures.Load("session-importProgress.json"), Fixtures.Load("session-imported.json")]);
        var id = await api.ImportRecordingAsync(path, startedAt, importParams.GetProperty("retain").GetBoolean());
        transport.AssertSent("session/import", importParams);
        Assert.Equal("a1b2c3d4e5f60718293a4b5c6d7e8f90", id);
        Assert.Equal(TimeSpan.FromSeconds(30), transport.Timeout);

        transport.Then.Clear();
        transport.Then.Add(Fixtures.Load("session-importFailed.json"));
        await Assert.ThrowsAsync<ImportCancelledException>(() => api.ImportRecordingAsync(path, startedAt, true));

        transport.Then.Clear();
        transport.Then.Add(JsonSerializer.SerializeToElement(new
        {
            method = "session/importFailed",
            @params = new { sessionId = "a1b2c3d4e5f60718293a4b5c6d7e8f90", error = "this file is not a sound recording" },
        }));
        var failed = await Assert.ThrowsAsync<EngineErrorException>(() => api.ImportRecordingAsync(path, startedAt, true));
        Assert.Equal("this file is not a sound recording", failed.Message);
    }

    [Fact]
    public async Task AMissingModelIsNamedByReadinessAndRefusesTheStart()
    {
        var transport = new ReplayingTransport();
        var api = new EngineApi(transport);

        transport.Reply = Fixtures.Load("engine-readiness.json").GetProperty("response").GetProperty("result");
        var readiness = await api.ReadinessAsync();
        Assert.Equal("engine/readiness", transport.Method);
        Assert.Equal(["asr", "diarisation", "segmentation"], readiness.Missing);
        Assert.True(readiness.Ready);

        // An older engine without the field reports nothing missing
        transport.Reply = JsonSerializer.SerializeToElement(new { firstUse = false, ready = true, strayNoteHost = false });
        Assert.Empty((await api.ReadinessAsync()).Missing);

        var refused = Fixtures.Load("session-start-refused.json");
        transport.Reply = JsonSerializer.SerializeToElement(new { sessionId = "s1" });
        await api.StartSessionAsync(true, "");
        transport.AssertSent("session/start", refused.GetProperty("request").GetProperty("params"));
        var error = refused.GetProperty("response").GetProperty("error");
        var thrown = new EngineErrorException(
            error.GetProperty("code").GetInt32(), error.GetProperty("message").GetString()!,
            error.GetProperty("data"));
        Assert.Equal("the speech recognition and speaker recognition models are not installed", thrown.Message);
    }

    [Fact]
    public void AnEngineErrorCarriesTheReasonFromItsData()
    {
        var error = Fixtures.Load("session-error.json").GetProperty("error");
        var thrown = new EngineErrorException(
            error.GetProperty("code").GetInt32(), error.GetProperty("message").GetString()!,
            error.GetProperty("data"));
        Assert.Equal("no session nope", thrown.Message);
        Assert.Equal("Invalid params", new EngineErrorException(-32602, "Invalid params", null).Message);
    }

    [Fact]
    public async Task EngineAnchorAndSessionRequestsAreWhatTheClientSends()
    {
        const string session = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
        var transport = new ReplayingTransport();
        var api = new EngineApi(transport);
        var calls = new (string Fixture, Func<Task> Call)[]
        {
            ("anchor-clear.json", () => api.ClearAnchorAsync()),
            ("anchor-enrol.json", () => api.StartEnrolmentAsync(45, "{0.0.1}.{aa}")),
            ("anchor-enrol-cancel.json", () => api.CancelEnrolmentAsync()),
            ("anchor-enrol-finish.json", () => api.FinishEnrolmentAsync()),
            ("asr-device.json", () => api.SetAsrDeviceAsync("NPU")),
            ("audio-inputs.json", () => api.ListAudioInputsAsync()),
            ("engine-exit.json", () => api.RequestExitAsync()),
            ("engine-metrics.json", () => api.MetricsAsync()),
            ("guidance-documents-remove.json", () => api.RemoveDocumentAsync(7302914125883421)),
            ("guidance-documents-removeAll.json", () => api.RemoveAllDocumentsAsync()),
            ("guidance-research.json", () => api.SetResearchGuidanceAsync(true)),
            ("note-options.json", () => api.SetNoteOptionsAsync("soap", "concise")),
            ("note-regenerate.json", () => api.RegenerateNoteAsync("prose", "detailed")),
            ("note-update.json", () => api.UpdateNoteAsync(session, "Swollen left elbow for a week.\nNo injury.")),
            ("patient-regenerate.json", () => api.RegeneratePatientAsync()),
            ("patient-translate.json", () => api.TranslatePatientAsync(session, "Polish")),
            ("patient-update.json", () => api.UpdatePatientAsync(session, "Rest the elbow and take ibuprofen with food.")),
            ("reflection-delete.json", () => api.DeleteReflectionAsync(session)),
            ("reflection-summary-request.json", () => api.SummariseReflectionAsync(session)),
            ("session-cancel.json", () => api.CancelSessionAsync()),
            ("session-delete.json", () => api.DeleteSessionAsync("0f1e2d3c4b5a69788796a5b4c3d2e1f0")),
            ("session-start.json", () => api.StartSessionAsync(true, "")),
            ("session-stop.json", () => api.StopSessionAsync()),
            ("translate-languages.json", () => api.LanguagesAsync()),
        };
        foreach (var (fixture, call) in calls)
        {
            var request = Fixtures.Load(fixture).GetProperty("request");
            transport.Reply = Fixtures.Load(fixture).GetProperty("response").GetProperty("result");
            await call();
            transport.AssertSent(request.GetProperty("method").GetString()!, ParamsOf(request));
        }
    }

    [Fact]
    public async Task EngineAnchorAndSessionRepliesParseIntoTheClientTypes()
    {
        var transport = new ReplayingTransport();
        var api = new EngineApi(transport);

        transport.Reply = Result("asr-device.json");
        Assert.Equal(new AsrDeviceState("NPU", "loading"), await api.SetAsrDeviceAsync("NPU"));

        transport.Reply = Result("audio-inputs.json");
        Assert.Equal(
            [
                new AudioInput("{0.0.1}.{aa}", "Microphone Array (Realtek(R) Audio)", "Microphone Array", true, false),
                new AudioInput("{0.0.1}.{bb}", "Headset (H800 Hands-Free)", "Headset", false, true),
            ],
            await api.ListAudioInputsAsync());

        transport.Reply = Result("engine-metrics.json");
        var metrics = await api.MetricsAsync();
        Assert.Equal(12.5, metrics.AsrRealtimeFactor);
        Assert.Equal(new EngineDevices("GPU.0", "GPU.0"), metrics.Devices);

        transport.Reply = Result("session-start.json");
        Assert.Equal("a1b2c3d4e5f60718293a4b5c6d7e8f90", await api.StartSessionAsync(true, ""));
        transport.Reply = Result("session-stop.json");
        Assert.Equal("a1b2c3d4e5f60718293a4b5c6d7e8f90", await api.StopSessionAsync());

        transport.Reply = Result("translate-languages.json");
        Assert.Equal(["French", "Polish", "Romanian"], await api.LanguagesAsync());
    }

    [Fact]
    public void EnrolmentNoteSheetTranslationAndSummaryNotificationsParse()
    {
        Assert.Equal(new EnrolmentProgress(0.5, 12.4, 9.1, false), Parse("anchor-progress.json"));
        Assert.Equal(new EnrolmentDone(true, ""), Parse("anchor-enrolled.json"));
        Assert.Equal(new AsrDeviceState("NPU", "ready"), Parse("asr-device-notification.json"));

        Assert.Equal(new NotePartial("Swollen left elbow", 17.2), Parse("note-partial.json"));
        Assert.Equal(new NoteReady("Swollen left elbow for a week. No injury.", 16.8), Parse("note-ready.json"));
        Assert.Equal(new NoteRefused("This does not sound like a consultation.", true), Parse("note-refused.json"));
        Assert.Equal(new NoteFailed("the note model stopped responding"), Parse("note-failed.json"));

        Assert.Equal(new PatientPartial("Rest the elbow", 17.2), Parse("patient-partial.json"));
        Assert.Equal(
            new PatientReady("Rest the elbow and take ibuprofen with food.", 16.8), Parse("patient-ready.json"));
        Assert.Equal(new PatientFailed("the note model stopped responding"), Parse("patient-failed.json"));

        Assert.Equal(new TranslationPartial("Odpoczywaj", 22.4), Parse("translate-partial.json"));
        Assert.Equal(
            new TranslationReady("Odpoczywaj łokieć i przyjmuj ibuprofen z jedzeniem.", "Polish", 21.9),
            Parse("translate-ready.json"));
        Assert.Equal(new TranslationFailed("translation failed"), Parse("translate-failed.json"));

        Assert.Equal(
            new ReflectionSummaryFailed("a1b2c3d4e5f60718293a4b5c6d7e8f90", "no stored note"),
            Parse("reflection-summaryFailed.json"));
    }

    // A method without parameters has no params member, and the transport sees null
    private static JsonElement ParamsOf(JsonElement request) =>
        request.TryGetProperty("params", out var parameters)
            ? parameters
            : JsonSerializer.SerializeToElement<object?>(null);

    private static JsonElement Result(string fixture) =>
        Fixtures.Load(fixture).GetProperty("response").GetProperty("result");

    private static EngineNotification? Parse(string fixture)
    {
        var notification = Fixtures.Load(fixture);
        return EngineNotifications.Parse(
            notification.GetProperty("method").GetString()!, notification.GetProperty("params"));
    }

    /// <summary>
    /// Records the last request and answers every one with the scripted reply, after sending the
    /// notifications in Then.
    /// </summary>
    private sealed class ReplayingTransport : IEngineTransport
    {
        private JsonElement _params;

        public event Action<string, JsonElement>? NotificationReceived;

        public List<JsonElement> Then { get; } = [];

        public event Action<bool>? ConnectedChanged
        {
            add { }
            remove { }
        }

        public bool Connected => true;

        public JsonElement Reply { get; set; } = JsonSerializer.SerializeToElement(new { });

        public TimeSpan Timeout { get; private set; }

        public string Method { get; private set; } = "";

        public Task<JsonElement> RequestAsync(
            string method, object? parameters, TimeSpan timeout, CancellationToken cancellationToken = default)
        {
            Method = method;
            Timeout = timeout;
            _params = JsonSerializer.SerializeToElement(parameters, Protocol.JsonOptions);
            foreach (var notification in Then)
            {
                NotificationReceived?.Invoke(
                    notification.GetProperty("method").GetString()!, notification.GetProperty("params"));
            }

            return Task.FromResult(Reply);
        }

        public void AssertSent(string method, JsonElement expected)
        {
            Assert.Equal(method, Method);
            Assert.True(JsonElement.DeepEquals(expected, _params), $"{method} sent {_params}");
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
