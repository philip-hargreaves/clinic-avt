using System.Security.Cryptography;
using System.Text;

namespace ClinicAVT.App.Core.Features.Backup;

/// <summary>The password a backup is locked with: a generated one, or the clinician's own.</summary>
public static class BackupPasswords
{
    public const int MinimumLength = 12;

    public const int GeneratedWords = 5;

    // The EFF large list has 7776 words, so five give about 64 bits
    private static readonly Lazy<string[]> Words = new(LoadWords);

    // Words a clinician should not be shown in a password. A redraw keeps the choice uniform
    private static readonly HashSet<string> Screened =
    [
        "abdomen", "abdominal", "aching", "agony", "anemia", "anemic", "bladder", "casket",
        "chemo", "choking", "coma", "coroner", "cough", "cramp", "diabetes", "diagnosis",
        "dosage", "drown", "fetal", "fever", "gangrene", "gauze", "gore", "gory", "grief",
        "groin", "liver", "lung", "mortician", "mortuary", "obituary", "ovary", "pelvis",
        "poison", "polio", "pregnant", "rash", "rectal", "relapse", "remission", "spleen",
        "sprain", "strangle", "surgery", "tremor", "virus", "widow", "womb", "wound",
    ];

    // Long enough to pass the length rule and still among the first tried
    private static readonly HashSet<string> Common =
    [
        "password1234", "password123!", "password12345", "passwordpassword", "123456789012",
        "1234567890ab", "qwertyuiopas", "qwerty123456", "qwertyuiop12", "abcdefghijkl",
        "iloveyou1234", "letmein12345", "welcome12345", "administrator", "aaaaaaaaaaaa",
        "111111111111", "000000000000", "clinicavt123", "clinicavt1234",
    ];

    /// <summary>Five words from the EFF large list, hyphenated so a screen reader says each.</summary>
    public static string Generate()
    {
        var words = Words.Value;
        var chosen = new string[GeneratedWords];
        for (var i = 0; i < chosen.Length; i++)
        {
            string word;
            do
            {
                word = words[RandomNumberGenerator.GetInt32(words.Length)];
            }
            while (Screened.Contains(word));

            chosen[i] = word;
        }

        return string.Join('-', chosen);
    }

    /// <summary>What is wrong with the clinician's own password, or empty. The second box counts once typed in.</summary>
    public static string Problem(string password, string again)
    {
        if (password.Length == 0)
        {
            return "";
        }

        // Counted as the engine counts, in characters after normalising
        if (password.Normalize(NormalizationForm.FormC).EnumerateRunes().Count() < MinimumLength)
        {
            return $"Use {MinimumLength} characters or more.";
        }

        if (Common.Contains(password.ToLowerInvariant()))
        {
            return "This password is too easy to guess. Choose another.";
        }

        return again.Length > 0 && again != password ? "The two passwords do not match." : "";
    }

    public static bool Acceptable(string password, string again) =>
        password.Length > 0 && again == password && Problem(password, again).Length == 0;

    // One word per line after its dice number
    private static string[] LoadWords()
    {
        using var stream = typeof(BackupPasswords).Assembly
            .GetManifestResourceStream("eff_large_wordlist.txt")
            ?? throw new InvalidOperationException("the word list is missing from the build");
        using var reader = new StreamReader(stream);
        return reader.ReadToEnd()
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(line => line[(line.IndexOf('\t') + 1)..])
            .ToArray();
    }
}
