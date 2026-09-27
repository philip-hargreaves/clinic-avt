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
        Assert.Equal(JsonValueKind.Null, close.GetProperty("params").ValueKind);

        var level = Fixtures.Load("audio-level.json");
        Assert.Equal("audio.level", level.GetProperty("method").GetString());
        Assert.Equal(0.5, level.GetProperty("params").GetProperty("level").GetDouble());
        Assert.False(level.GetProperty("params").GetProperty("clipped").GetBoolean());

        var start = Fixtures.Load("session-start-playback.json");
        Assert.Equal("session/start", start.GetProperty("method").GetString());
        Assert.False(string.IsNullOrEmpty(
            start.GetProperty("params").GetProperty("playback").GetProperty("id").GetString()));

        var playback = Fixtures.Load("audio-level-playback.json");
        Assert.Equal("audio.level", playback.GetProperty("method").GetString());
        Assert.Equal(271.4, playback.GetProperty("params").GetProperty("seconds").GetDouble());

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
        var translation = patient.GetProperty("translation");
        Assert.Equal("pl", translation.GetProperty("language").GetString());
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
}
