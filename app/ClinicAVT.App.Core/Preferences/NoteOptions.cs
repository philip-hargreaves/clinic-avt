namespace ClinicAVT.App.Core.Preferences;

/// <summary>The note options the engine accepts, in the order the note style menu lists them.</summary>
public static class NoteOptions
{
    public static readonly IReadOnlyList<NoteOption> Styles =
        [new("prose", "Prose"), new("soap", "SOAP")];

    // About 80 and 160 words. A saved "standard" falls back to concise
    public static readonly IReadOnlyList<NoteOption> Details =
        [new("concise", "Concise"), new("detailed", "Detailed")];

    public static NoteOption DefaultStyle => Styles[0];

    public static NoteOption DefaultDetail => Details[0];

    /// <summary>The style with that engine value, the default for anything else.</summary>
    public static NoteOption Style(string? value) => Find(Styles, value) ?? DefaultStyle;

    public static NoteOption Detail(string? value) => Find(Details, value) ?? DefaultDetail;

    private static NoteOption? Find(IReadOnlyList<NoteOption> options, string? value) =>
        value is null ? null : options.FirstOrDefault(o => o.Value == value);
}
