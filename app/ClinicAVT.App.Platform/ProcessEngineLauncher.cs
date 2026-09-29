using System.ComponentModel;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.System.Threading;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Platform;

/// <summary>
/// Starts the engine suspended and resumes it only after it joins the kill-on-close job, so no
/// child escapes the job. Stderr goes to a log file.
/// </summary>
public sealed class ProcessEngineLauncher(string exePath, string arguments = "",
    string? stderrPath = null, Func<IEnumerable<string>>? extraArguments = null)
    : IEngineLauncher, IDisposable
{
    private readonly JobObject _job = new();
    private bool _rotated;

    public IEngineProcess Launch()
    {
        // Extra arguments are read per launch, so a restart picks up changes
        var extra = string.Join(" ", (extraArguments?.Invoke() ?? []).Select(Quote));
        // CreateProcess may write into the command line, so it needs a buffer
        Span<char> commandLine =
            ($"\"{exePath}\" {arguments} {extra}".TrimEnd() + '\0').ToCharArray();

        using var stderr = OpenStderr();
        unsafe
        {
            var startup = new STARTUPINFOW { cb = (uint)sizeof(STARTUPINFOW) };
            if (stderr is not null)
            {
                startup.dwFlags = STARTUPINFOW_FLAGS.STARTF_USESTDHANDLES;
                startup.hStdError = new HANDLE(stderr.SafeFileHandle.DangerousGetHandle());
            }

            if (!PInvoke.CreateProcess(
                    exePath, ref commandLine, null, null, stderr is not null,
                    PROCESS_CREATION_FLAGS.CREATE_SUSPENDED | PROCESS_CREATION_FLAGS.CREATE_NO_WINDOW,
                    null,
                    Path.GetDirectoryName(exePath), in startup, out var info))
            {
                throw new Win32Exception();
            }

            var handle = new SafeProcessHandle((IntPtr)info.hProcess.Value, ownsHandle: true);
            try
            {
                _job.Assign(handle);
                if (OperatingSystem.IsWindowsVersionAtLeast(8))
                {
                    PowerThrottling.Disable(handle);  // the engine repeats this on itself
                }
                if (PInvoke.ResumeThread(info.hThread) == uint.MaxValue)
                {
                    throw new Win32Exception();
                }
            }
            catch
            {
                // A suspended orphan would hang forever, so take it down here
                PInvoke.TerminateProcess(info.hProcess, 1);
                handle.Dispose();
                throw;
            }
            finally
            {
                PInvoke.CloseHandle(info.hThread);
            }

            return new EngineProcess(handle, info.dwProcessId);
        }
    }

    public IEngineProcess? Adopt()
    {
        if (PipeTransport.ServingProcessId(EngineInfo.PipeName, TimeSpan.FromSeconds(2))
            is not uint pid)
        {
            return null;
        }

        using var opened = PInvoke.OpenProcess_SafeHandle(
            PROCESS_ACCESS_RIGHTS.PROCESS_SYNCHRONIZE
            | PROCESS_ACCESS_RIGHTS.PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (opened.IsInvalid)
        {
            return null;
        }

        var handle = new SafeProcessHandle(opened.DangerousGetHandle(), ownsHandle: true);
        opened.SetHandleAsInvalid();
        // Reject another program holding the pipe
        if (!string.Equals(ImagePath(handle), Path.GetFullPath(exePath),
                StringComparison.OrdinalIgnoreCase))
        {
            handle.Dispose();
            return null;
        }

        try
        {
            _job.Assign(handle);
        }
        catch (Win32Exception)
        {
            // Already in another job. It is still watched through the handle
        }

        return new EngineProcess(handle, pid);
    }

    public void Release() => _job.KeepProcessesOnClose();

    private static string? ImagePath(SafeProcessHandle process)
    {
        Span<char> buffer = stackalloc char[1024];
        var length = (uint)buffer.Length;
        return PInvoke.QueryFullProcessImageName(
            process, PROCESS_NAME_FORMAT.PROCESS_NAME_WIN32, buffer, ref length)
            ? new string(buffer[..(int)length])
            : null;
    }

    // The file is appended across launches and rotated once it is large. The handle
    // must be inheritable for CreateProcess to pass it on
    private FileStream? OpenStderr()
    {
        if (stderrPath is null)
        {
            return null;
        }

        if (!_rotated)
        {
            LogRotation.Rotate(stderrPath, keep: 12);
            _rotated = true;
        }

        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(stderrPath)!);
            var stream = new FileStream(
                stderrPath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
            PInvoke.SetHandleInformation(
                stream.SafeFileHandle, (uint)HANDLE_FLAGS.HANDLE_FLAG_INHERIT,
                HANDLE_FLAGS.HANDLE_FLAG_INHERIT);
            return stream;
        }
        catch (IOException)
        {
            return null;
        }
    }

    private static string Quote(string argument) =>
        argument.Contains(' ') ? $"\"{argument}\"" : argument;

    public void Dispose() => _job.Dispose();
}
