using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Backup;

public static class BackupWords
{
    public const string Includes =
        "Includes transcripts, notes, patient information, translations and reflections. Audio is not included.";

    public const string ReflectionsIncludes =
        "Includes reflections, with each consultation's title and summary. Transcripts, notes, "
        + "patient information and audio are not included.";

    public const string RestoreCaption =
        "Choose the backup file and enter its password. Nothing is added until you confirm. Restore "
        + "only on a computer your practice has approved for patient information.";

    public const string PasswordNote =
        "Save this password somewhere safe. Without it, this backup can't be opened.";

    public const string ReflectionsTick = "Delete associated reflections";

    /// <summary>"Saved as b in Documents.", naming the drive for a root such as a USB stick.</summary>
    public static string SavedLine(string path)
    {
        var folder = Path.GetDirectoryName(path) ?? "";
        var name = Path.GetFileName(folder.TrimEnd(Path.DirectorySeparatorChar));
        return $"Saved as {Path.GetFileNameWithoutExtension(path)} in {(name.Length > 0 ? name : folder)}.";
    }

    /// <summary>A failed backup or restore in plain words, from the engine's fixed code.</summary>
    public static string Failure(ArchiveJob job, ArchiveError code)
    {
        var backup = job == ArchiveJob.Backup;
        return code switch
        {
            ArchiveError.WrongPassword => "The password does not match this backup.",
            ArchiveError.NotABackup => "This is not a ClinicAVT backup.",
            ArchiveError.NewerVersion =>
                "This backup was made by a newer version of ClinicAVT. Update ClinicAVT, then try again.",
            ArchiveError.Damaged when backup =>
                "The backup did not pass its check, so it was not kept. Try again, or save it somewhere else. "
                + "Nothing on this computer has changed.",
            ArchiveError.Damaged => "This file is damaged or incomplete. Nothing was restored.",
            ArchiveError.WeakPassword => $"Use a password of {BackupPasswords.MinimumLength} characters or more.",
            ArchiveError.WriteFailed when backup =>
                "The backup could not be saved there. Check there is space and that you can save to "
                + "that folder, then try again. Nothing on this computer has changed.",
            ArchiveError.WriteFailed =>
                "Restoring stopped before the end. Restore again to finish; consultations already "
                + "here are left as they are.",
            ArchiveError.ReadFailed => "The file could not be read. Check the drive is still connected, then try again.",
            _ when backup => "The backup could not be made. Nothing on this computer has changed.",
            _ => "The backup could not be restored. Nothing was restored.",
        };
    }

    /// <summary>A request the engine refused before the job started, such as during a recording.</summary>
    public static string Refused(ArchiveJob job, Exception e, ILogger logger) =>
        job == ArchiveJob.Backup
            ? $"The backup could not start: {EngineWords.Reason(e, logger)}. Nothing on this computer has changed."
            : $"The backup could not be opened: {EngineWords.Reason(e, logger)}.";

    /// <summary>
    /// The standing line for a folder that syncs to OneDrive, empty for any other. A work
    /// OneDrive may be approved by the practice; a personal one is not.
    /// </summary>
    public static string OneDriveLine(string folder, IOneDriveFolders oneDrive)
    {
        if (InRoot(folder, oneDrive.Work))
        {
            return "This folder is in your work OneDrive, so the backup will be copied there. "
                + "Check that your practice allows this.";
        }

        if (InRoot(folder, oneDrive.Personal))
        {
            return "This folder is in a personal OneDrive, so the backup will be copied to the "
                + "cloud. Choose a place your practice has approved.";
        }

        return InRoot(folder, oneDrive.Primary)
            ? "This folder is in OneDrive, so the backup will be copied to the cloud. Check that "
                + "your practice allows this."
            : "";
    }

    private static bool InRoot(string folder, string? root) =>
        !string.IsNullOrEmpty(root) && OneDrive.Holds(folder, [root]);
}
