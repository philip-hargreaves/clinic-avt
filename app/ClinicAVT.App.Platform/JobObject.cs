using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.System.JobObjects;

namespace ClinicAVT.App.Platform;

/// <summary>
/// A Windows Job Object with kill-on-close. Disposing it terminates every
/// assigned process, and the kernel closes the handle when this process dies
/// for any reason, so an assigned engine cannot outlive its owner.
/// </summary>
public sealed class JobObject : IDisposable
{
    private readonly SafeFileHandle _handle;

    public JobObject()
    {
        _handle = PInvoke.CreateJobObject(null, (string?)null);
        if (_handle.IsInvalid)
        {
            throw new Win32Exception();
        }

        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

        unsafe
        {
            if (!PInvoke.SetInformationJobObject(
                    Handle, JOBOBJECTINFOCLASS.JobObjectExtendedLimitInformation, &info,
                    (uint)sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)))
            {
                throw new Win32Exception();
            }
        }
    }

    private HANDLE Handle => new(_handle.DangerousGetHandle());

    /// <summary>Ends kill-on-close, so the assigned processes can finish after this one.</summary>
    public void KeepProcessesOnClose()
    {
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        unsafe
        {
            PInvoke.SetInformationJobObject(
                Handle, JOBOBJECTINFOCLASS.JobObjectExtendedLimitInformation, &info,
                (uint)sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
        }
    }

    public void Assign(SafeHandle process)
    {
        if (!PInvoke.AssignProcessToJobObject(Handle, new HANDLE(process.DangerousGetHandle())))
        {
            throw new Win32Exception();
        }
    }

    public void Dispose() => _handle.Dispose();
}
