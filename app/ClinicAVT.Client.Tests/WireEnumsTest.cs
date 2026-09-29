using System.Text.Json;

namespace ClinicAVT.Client.Tests;

public class WireEnumsTest
{
    // The strings the engine sends and accepts, spelled out so a renamed member fails here
    public static TheoryData<object, string> Spellings() => new()
    {
        { ModelState.Idle, "idle" },
        { ModelState.Loading, "loading" },
        { ModelState.Ready, "ready" },
        { ModelState.Failed, "failed" },
        { ModelTask.Asr, "asr" },
        { ModelTask.Note, "note" },
        { AsrDevice.Gpu, "GPU" },
        { AsrDevice.Npu, "NPU" },
        { CorporaState.Loading, "loading" },
        { CorporaState.Ready, "ready" },
        { CorporaState.Unavailable, "unavailable" },
        { DocumentState.Indexing, "indexing" },
        { DocumentState.Ready, "ready" },
        { DocumentState.Failed, "failed" },
        { DocumentState.Removed, "removed" },
        { DocumentError.PatientData, "patientData" },
        { DocumentError.Password, "password" },
        { DocumentError.NoText, "noText" },
        { IngestPhase.Paused, "paused" },
        { IngestPhase.Reading, "reading" },
        { IngestPhase.Preparing, "preparing" },
        { FinaliseStage.Transcript, "transcript" },
        { FinaliseStage.Speakers, "speakers" },
        { FinaliseStage.Turns, "turns" },
        { ImportStage.Reading, "reading" },
        { ImportStage.Speech, "speech" },
        { ImportStage.Transcribing, "transcribing" },
        { ImportStage.Finalising, "finalising" },
        { AnchorOrigin.None, "none" },
        { AnchorOrigin.Accrued, "accrued" },
        { AnchorOrigin.Enrolled, "enrolled" },
        { ArchiveJob.Backup, "backup" },
        { ArchiveJob.Restore, "restore" },
        { ArchivePhase.Writing, "writing" },
        { ArchivePhase.Checking, "checking" },
        { ArchiveError.WrongPassword, "wrong-password" },
        { ArchiveError.NotABackup, "not-a-backup" },
        { ArchiveError.NewerVersion, "newer-version" },
        { ArchiveError.Damaged, "damaged" },
        { ArchiveError.WeakPassword, "weak-password" },
        { ArchiveError.WriteFailed, "write-failed" },
        { ArchiveError.ReadFailed, "read-failed" },
    };

    [Theory]
    [MemberData(nameof(Spellings))]
    public void EachMemberReadsAndWritesAsTheEngineSpellsIt(object value, string wire)
    {
        var json = JsonSerializer.Serialize(value, value.GetType(), Protocol.JsonOptions);
        Assert.Equal($"\"{wire}\"", json);
        Assert.Equal(value, JsonSerializer.Deserialize(json, value.GetType(), Protocol.JsonOptions));
    }

    [Fact]
    public void AnUnknownOrMissingValueReadsAsTheDefaultAndANumberIsRejected()
    {
        Assert.Equal(ModelState.Unknown, JsonSerializer.Deserialize<ModelState>("\"compiling\"", Protocol.JsonOptions));
        Assert.Equal(DocumentError.Other, JsonSerializer.Deserialize<DocumentError>("null", Protocol.JsonOptions));
        Assert.Throws<JsonException>(() => JsonSerializer.Deserialize<ArchiveJob>("1", Protocol.JsonOptions));
    }
}
