using System.Text;

namespace ClinicAVT.App.Core.Features.Backup;

public static class BackupPasswords
{
    public const int MinimumLength = 8;

    // Long enough to pass the length rule and still among the first tried
    private static readonly HashSet<string> Common =
    [
        "password", "password1", "password12", "password123", "password1234", "passw0rd", "p@ssw0rd",
        "12345678", "123456789", "1234567890", "87654321", "11111111", "00000000", "aaaaaaaa",
        "abcd1234", "abcdefgh", "qwertyui", "qwertyuiop", "qwerty123", "qwerty1234", "1q2w3e4r",
        "zaq12wsx", "iloveyou", "iloveyou1", "sunshine", "football", "baseball", "princess",
        "welcome1", "welcome123", "letmein1", "letmein123", "trustno1", "changeme", "administrator",
        "clinicavt", "clinicavt1", "clinicavt123",
    ];

    /// <summary>What is wrong, or empty. A mismatch counts once the second box has text.</summary>
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
}
