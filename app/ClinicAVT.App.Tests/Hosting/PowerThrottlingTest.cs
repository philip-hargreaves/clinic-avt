using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using ClinicAVT.App.Platform;

namespace ClinicAVT.App.Tests.Hosting;

public class PowerThrottlingTest
{
    private const int ProcessPowerThrottling = 4;
    private const uint CurrentVersion = 1;
    private const uint ExecutionSpeed = 1;

    [StructLayout(LayoutKind.Sequential)]
    private struct ThrottlingState
    {
        public uint Version;
        public uint ControlMask;
        public uint StateMask;
    }

    [Fact]
    public void TheCurrentProcessIsOptedOut()
    {
        if (!OperatingSystem.IsWindowsVersionAtLeast(8))
        {
            return;
        }

        using var process = Process.GetCurrentProcess();

        Assert.True(PowerThrottling.Disable(process.SafeHandle));

        var state = new ThrottlingState { Version = CurrentVersion };
        Assert.True(GetProcessInformation(
            process.SafeHandle, ProcessPowerThrottling, ref state,
            (uint)Marshal.SizeOf<ThrottlingState>()));
        Assert.Equal(ExecutionSpeed, state.ControlMask & ExecutionSpeed);  // decided, not left to Windows
        Assert.Equal(0u, state.StateMask & ExecutionSpeed);  // and decided off
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetProcessInformation(
        SafeProcessHandle process, int informationClass, ref ThrottlingState information, uint size);
}
