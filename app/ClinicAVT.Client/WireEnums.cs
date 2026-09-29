using System.Reflection;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ClinicAVT.Client;

/// <summary>A model lane or device move, as note/model, note/tier and asr/device report it.</summary>
[JsonConverter(typeof(WireEnumConverter<ModelState>))]
public enum ModelState
{
    Unknown,
    Idle,
    Loading,
    Ready,
    Failed,
}

[JsonConverter(typeof(WireEnumConverter<ModelTask>))]
public enum ModelTask
{
    Other,
    Asr,
    Note,
}

[JsonConverter(typeof(WireEnumConverter<AsrDevice>))]
public enum AsrDevice
{
    Unknown,
    [JsonStringEnumMemberName("GPU")]
    Gpu,
    [JsonStringEnumMemberName("NPU")]
    Npu,
}

/// <summary>Whether the guidance embedder has loaded.</summary>
[JsonConverter(typeof(WireEnumConverter<CorporaState>))]
public enum CorporaState
{
    Unknown,
    Loading,
    Ready,
    Unavailable,
}

[JsonConverter(typeof(WireEnumConverter<DocumentState>))]
public enum DocumentState
{
    Unknown,
    Indexing,
    Ready,
    Failed,
    Removed,
}

/// <summary>Why an added document could not be searched.</summary>
[JsonConverter(typeof(WireEnumConverter<DocumentError>))]
public enum DocumentError
{
    Other,
    PatientData,
    Password,
    NoText,
}

/// <summary>A document ingest step, as guidance/progress reports it.</summary>
[JsonConverter(typeof(WireEnumConverter<IngestPhase>))]
public enum IngestPhase
{
    Unknown,
    Paused,
    Reading,
    Preparing,
}

/// <summary>A finalise stage, as session/progress reports it.</summary>
[JsonConverter(typeof(WireEnumConverter<FinaliseStage>))]
public enum FinaliseStage
{
    Unknown,
    Transcript,
    Speakers,
    Turns,
}

[JsonConverter(typeof(WireEnumConverter<ImportStage>))]
public enum ImportStage
{
    Unknown,
    Reading,
    Speech,
    Transcribing,
    Finalising,
}

/// <summary>Where the clinician's voiceprint came from.</summary>
[JsonConverter(typeof(WireEnumConverter<AnchorOrigin>))]
public enum AnchorOrigin
{
    Unknown,
    None,
    Accrued,
    Enrolled,
}

[JsonConverter(typeof(WireEnumConverter<ArchiveJob>))]
public enum ArchiveJob
{
    Unknown,
    Backup,
    Restore,
}

[JsonConverter(typeof(WireEnumConverter<ArchivePhase>))]
public enum ArchivePhase
{
    Unknown,
    Writing,
    Checking,
}

/// <summary>The fixed codes archive/failed carries.</summary>
[JsonConverter(typeof(WireEnumConverter<ArchiveError>))]
public enum ArchiveError
{
    Unknown,
    [JsonStringEnumMemberName("wrong-password")]
    WrongPassword,
    [JsonStringEnumMemberName("not-a-backup")]
    NotABackup,
    [JsonStringEnumMemberName("newer-version")]
    NewerVersion,
    [JsonStringEnumMemberName("damaged")]
    Damaged,
    [JsonStringEnumMemberName("weak-password")]
    WeakPassword,
    [JsonStringEnumMemberName("write-failed")]
    WriteFailed,
    [JsonStringEnumMemberName("read-failed")]
    ReadFailed,
}

/// <summary>
/// An enum as the wire spells it: the camel-case member name, or the name its
/// JsonStringEnumMemberName gives. A string this build does not know, or null, reads as the
/// default member.
/// </summary>
public sealed class WireEnumConverter<T> : JsonConverter<T>
    where T : struct, Enum
{
    private static readonly Dictionary<T, string> Names = Enum.GetValues<T>().ToDictionary(
        value => value,
        value => typeof(T).GetField(value.ToString())!.GetCustomAttribute<JsonStringEnumMemberNameAttribute>()?.Name
            ?? JsonNamingPolicy.CamelCase.ConvertName(value.ToString()));

    private static readonly Dictionary<string, T> Values =
        Names.ToDictionary(pair => pair.Value, pair => pair.Key, StringComparer.Ordinal);

    public override T Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
        reader.TokenType switch
        {
            JsonTokenType.Null => default,
            JsonTokenType.String => Values.GetValueOrDefault(reader.GetString()!),
            _ => throw new JsonException($"{typeof(T).Name}: expected a string"),
        };

    public override void Write(Utf8JsonWriter writer, T value, JsonSerializerOptions options) =>
        writer.WriteStringValue(Names[value]);
}
