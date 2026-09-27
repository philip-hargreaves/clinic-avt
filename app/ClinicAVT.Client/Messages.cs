using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Unicode;

namespace ClinicAVT.Client;

public sealed record PeerInfo(string Name, string Version, int ProtocolVersion);

public static class Protocol
{
    public const int ProtocolVersion = 1;

    // Non-ASCII patient and drug names go on the wire as readable UTF-8 without \uXXXX escapes
    public static JsonSerializerOptions JsonOptions { get; } = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        Encoder = JavaScriptEncoder.Create(UnicodeRanges.All),
    };

    /// <summary>An object payload as a record, or null for anything else or a field of the wrong kind.</summary>
    public static T? Parse<T>(JsonElement element)
        where T : class
    {
        if (element.ValueKind != JsonValueKind.Object)
        {
            return null;
        }

        try
        {
            return element.Deserialize<T>(JsonOptions);
        }
        catch (JsonException)
        {
            return null;
        }
    }
}

/// <summary>Reading a reply without trusting its shape.</summary>
public static class JsonElements
{
    /// <summary>The named property when the element is an object and the property is of that kind.</summary>
    public static bool TryProperty(this JsonElement element, string name, JsonValueKind kind, out JsonElement value)
    {
        if (element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out value)
            && value.ValueKind == kind)
        {
            return true;
        }

        value = default;
        return false;
    }

    /// <summary>The element down a path of object keys, null where the path breaks.</summary>
    public static JsonElement? Find(this JsonElement root, params string[] path)
    {
        var current = root;
        foreach (var key in path)
        {
            if (current.ValueKind != JsonValueKind.Object || !current.TryGetProperty(key, out current))
            {
                return null;
            }
        }

        return current;
    }

    public static double? Number(this JsonElement root, params string[] path) =>
        root.Find(path) is { ValueKind: JsonValueKind.Number } n ? n.GetDouble() : null;

    public static string? Text(this JsonElement root, params string[] path) =>
        root.Find(path) is { ValueKind: JsonValueKind.String } s ? s.GetString() : null;
}

/// <summary>
/// A JSON-RPC error response from the engine. The message is the engine's reason from the error
/// data when it sent one, since the error message itself only names the kind.
/// </summary>
public sealed class EngineErrorException(int code, string message, JsonElement? data)
    : Exception(data is { ValueKind: JsonValueKind.String } reason
        && reason.GetString() is { Length: > 0 } text ? text : message)
{
    public int Code { get; } = code;

    public JsonElement? ErrorData { get; } = data;
}
