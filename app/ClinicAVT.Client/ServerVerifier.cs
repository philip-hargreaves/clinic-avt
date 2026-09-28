using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace ClinicAVT.Client;

/// <summary>
/// The pid of the process serving a named pipe. Comparing it with the pid the
/// launcher started needs no signature and cannot be spoofed by renaming a binary.
/// </summary>
[SupportedOSPlatform("windows")]
internal static partial class ServerVerifier
{
    public static uint? GetServerProcessId(NamedPipeClientStream pipe) =>
        GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out var pid)
            ? pid
            : null;

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetNamedPipeServerProcessId(nint pipeHandle, out uint serverPid);
}
