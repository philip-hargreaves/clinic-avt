using System.Text.Json;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.Support;

/// <summary>Wire-shaped guidance corpora, results and the records built from them.</summary>
internal static class GuidanceRecords
{
    public const long DocumentId = 6368831970660585267L;

    public static void ApplyReady(this GuidanceViewModel guidance, JsonElement record) =>
        guidance.ApplyReady(Protocol.Parse<GuidanceRecord>(record)!);

    /// <summary>The fixture corpus as guidance/corpora lists it and a search names it.</summary>
    public static object Corpus(
        string? label = null, string id = "fixture", string name = "Fixture guidance corpus",
        string attribution = "none") => new
        {
            id,
            name,
            licence = "invented",
            attribution,
            label,
            source = "text",
            embedder = "gte-large-int8",
            sha256 = "",
            chunks = 40,
            builtAt = "2026-09-11T00:00:00Z",
            unavailable = (string?)null,
        };

    /// <summary>An added PDF as a corpus of its own.</summary>
    public static object DocumentCorpus(long document = DocumentId) => new
    {
        id = $"upload:{document}",
        name = "BSR PMR guidelines 2009",
        licence = "",
        attribution = "",
        source = "upload",
        embedder = "gte-large-int8",
        sha256 = "",
        chunks = 12,
        builtAt = "2026-09-15T09:13:02Z",
        unavailable = (string?)null,
    };

    /// <summary>
    /// One guideline recommendation. The code is the chunk id's first part unless given.
    /// </summary>
    public static object Result(
        string chunkId, string trigger = "A sentence of the note.",
        string url = "https://example.test/fx100", string source = "nice",
        string number = "1.1.1", string section = "1.1 Referral", string updateTag = "",
        string lastUpdated = "", string corpus = "fixture", string? code = null,
        string title = "Fictional guideline", string citation = "FX100 1.1.1, Fictional guideline",
        long document = 0, int page = 0, int pages = 0) => new
        {
            corpus,
            chunkId,
            code = code ?? chunkId.Split('-')[0],
            number,
            title,
            section,
            text = "Refer adults with persistent synovitis.",
            url,
            lastUpdated,
            updateTag,
            source,
            citation,
            score = 0.9,
            trigger,
            document,
            page,
            pages,
        };

    /// <summary>One passage of an added PDF.</summary>
    public static object DocumentResult(
        int page = 1, int pages = 5, string number = "1.2", long document = DocumentId) =>
        Result($"upload:{document}-4", trigger: "", url: "", source: "upload", number: number, section: "",
            lastUpdated: "2026-09-15T09:12:44Z", corpus: $"upload:{document}", code: "",
            title: "BSR PMR guidelines 2009",
            citation: "BSR PMR guidelines 2009, page 2, 1.2 (added 15 Sep 2026)",
            document: document, page: page, pages: pages);

    public static GuidanceResult Parse(object result) =>
        Protocol.Parse<GuidanceResult>(JsonSerializer.SerializeToElement(result))!;

    public static GuidanceRecommendation Found(object result, string sourceLabel = "NICE", bool labelled = true) =>
        GuidanceRecommendation.From(Parse(result), sourceLabel, true, labelled);

    /// <summary>
    /// A guidance/ready payload. Id is set for the note's search and null for a typed query.
    /// </summary>
    public static JsonElement Ready(string? id, object[] shown, bool searched = true,
        bool? stale = false, string? storeError = null, object[]? corpora = null) =>
        JsonSerializer.SerializeToElement(new
        {
            id,
            storeError,
            stale,
            version = 1,
            noteRevision = 1,
            shown,
            searched = corpora ?? (searched ? [Corpus("NICE", attribution: "Fixture attribution")] : []),
            considered = shown.Length,
            floor = 0.85,
            abstained = shown.Length == 0,
        });

    public static JsonElement Failed(string? id, string detail) =>
        JsonSerializer.SerializeToElement(new { id, detail });

    /// <summary>The record stored with a note, as session/guidance returns it.</summary>
    public static JsonElement Record(
        object[] shown, bool stale = false, bool documentsChanged = false, object[]? searched = null) =>
        JsonSerializer.SerializeToElement(new
        {
            version = 1,
            noteRevision = 1,
            generatedAt = "2026-09-13T01:00:00Z",
            stale,
            documentsChanged,
            shown,
            searched = searched ?? [Corpus("NICE", attribution: "Fixture attribution")],
            considered = shown.Length,
            floor = 0.85,
            abstained = shown.Length == 0,
        });
}
